// core-keys M1 mule — CTAPHID transport + a minimal CTAP2 responder.
//
// Scope for M1: enough of CTAPHID (INIT, PING, CBOR, MSG, CANCEL, WINK) and
// CTAP2 (authenticatorGetInfo) that a host — Chrome's WebAuthn stack, libfido2,
// `fido2-token -I` — recognises the device as a real authenticator over the
// standard interface. Credential operations, PIN, and the desktop co-auth
// handshake are M1/M2 work; unsupported commands return spec error codes rather
// than lying. Single active channel/transaction: sufficient for the mule, and a
// deliberate simplification over the full multi-channel CTAPHID router.
#include <string.h>
#include "esp_log.h"
#include "corekeys.h"
#include "ctaphid.h"
#include "ctap2.h"
#include "lcd.h"
#include "ui.h"

static const char *TAG = "ctaphid";

// ---- CTAPHID constants -------------------------------------------------------
#define CTAPHID_BROADCAST_CID  0xFFFFFFFFu
#define CTAPHID_INIT_NONCE_LEN 8
#define CTAPHID_MAX_PAYLOAD    7609   // 57 + 128*59, the CTAPHID maximum message

// Command bytes (transport sets bit 7 on the wire; we mask it off).
#define CTAPHID_PING    0x01
#define CTAPHID_MSG     0x03
#define CTAPHID_LOCK    0x04
#define CTAPHID_INIT    0x06
#define CTAPHID_WINK    0x08
// CTAPHID_CBOR / CTAPHID_KEEPALIVE come from ctaphid.h (shared with ctap2.c).
#define CTAPHID_CANCEL  0x11
#define CTAPHID_ERROR   0x3F

// CTAPHID error codes.
#define ERR_INVALID_CMD 0x01
#define ERR_INVALID_LEN 0x03
#define ERR_INVALID_SEQ 0x04
#define ERR_CHANNEL_BUSY 0x06
#define ERR_OTHER       0x7F

// CTAPHID capability flags.
#define CAP_WINK 0x01
#define CAP_CBOR 0x04
#define CAP_NMSG 0x08

// CTAP2 status / command bytes.
#define CTAP2_OK                   0x00
#define CTAP1_ERR_INVALID_CMD      0x01
#define CTAP2_CMD_MAKE_CREDENTIAL  0x01
#define CTAP2_CMD_GET_ASSERTION    0x02
#define CTAP2_CMD_GET_INFO         0x04

// ---- Reassembly state (single active transaction) ----------------------------
static struct {
    bool     active;
    uint32_t cid;
    uint8_t  cmd;
    uint16_t bcnt;     // declared total payload length
    uint16_t off;      // bytes assembled so far
    uint8_t  seq;      // next expected continuation sequence number
    uint8_t  buf[CTAPHID_MAX_PAYLOAD];
} rx;

static uint32_t next_cid = 1;   // allocated on INIT (broadcast)

// ---- Big-endian helpers ------------------------------------------------------
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// ---- Response fragmenter -----------------------------------------------------
// Frame `data` as a CTAPHID message (init packet + continuation packets), each a
// zero-padded 64-byte report, and send them in order on the CTAP interface.
void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t pkt[CK_REPORT_SIZE];
    uint16_t off = 0;

    // Init packet: CID | CMD(0x80|cmd) | BCNTH | BCNTL | data[0..56]
    memset(pkt, 0, sizeof(pkt));
    put_be32(pkt, cid);
    pkt[4] = 0x80 | cmd;
    pkt[5] = (uint8_t)(len >> 8);
    pkt[6] = (uint8_t)(len & 0xFF);
    uint16_t n = len < 57 ? len : 57;
    if (n) memcpy(&pkt[7], data, n);
    ck_report_send(ITF_CTAP, pkt);
    off = n;

    // Continuation packets: CID | SEQ | data[...]
    uint8_t seq = 0;
    while (off < len) {
        memset(pkt, 0, sizeof(pkt));
        put_be32(pkt, cid);
        pkt[4] = seq++;
        n = (len - off) < 59 ? (len - off) : 59;
        memcpy(&pkt[5], &data[off], n);
        ck_report_send(ITF_CTAP, pkt);
        off += n;
    }
}

static void ctaphid_error(uint32_t cid, uint8_t code)
{
    ctaphid_send(cid, CTAPHID_ERROR, &code, 1);
}

void ctaphid_keepalive(uint32_t cid, uint8_t status)
{
    ctaphid_send(cid, CTAPHID_KEEPALIVE, &status, 1);
}

// ---- CTAP2: authenticatorGetInfo --------------------------------------------
// Pinless / touch-only posture: options {rk:false, up:true} with NEITHER "uv"
// NOR "clientPin" present, so hosts never prompt to set a PIN. AAGUID is 16 zero
// bytes (self/none convention) and BYTE-IDENTICAL to every makeCredential
// attestedCredentialData. Advertises ES256 (-7) and the usb transport.
static const uint8_t GET_INFO_RESPONSE[] = {
    CTAP2_OK,
    0xA5,                                     // map(5)
      0x01,                                   //  1: versions
        0x82,
          0x68, 'F','I','D','O','_','2','_','1',
          0x68, 'F','I','D','O','_','2','_','0',
      0x03,                                   //  3: aaguid = 16 zero bytes
        0x50, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
      0x04,                                   //  4: options
        0xA2,
          0x62, 'r','k', 0xF4,                //    "rk": false (non-discoverable)
          0x62, 'u','p', 0xF5,                //    "up": true  (user presence)
      0x09,                                   //  9: transports
        0x81, 0x63, 'u','s','b',
      0x0A,                                   //  10: algorithms
        0x81,
          0xA2,
            0x63, 'a','l','g', 0x26,          //    "alg": -7 (ES256)
            0x64, 't','y','p','e',
              0x6A, 'p','u','b','l','i','c','-','k','e','y',
};

static void ctap2_dispatch(uint32_t cid, const uint8_t *data, uint16_t len)
{
    if (len < 1) { ctaphid_error(cid, ERR_INVALID_LEN); return; }
    uint8_t cmd = data[0];
    switch (cmd) {
    case CTAP2_CMD_GET_INFO:
        ESP_LOGI(TAG, "CBOR authenticatorGetInfo");
        ui_note_ctap("getInfo");
        ctaphid_send(cid, CTAPHID_CBOR, GET_INFO_RESPONSE, sizeof(GET_INFO_RESPONSE));
        break;
    case CTAP2_CMD_MAKE_CREDENTIAL:
        ESP_LOGI(TAG, "CBOR authenticatorMakeCredential");
        ctap2_make_credential(cid, data + 1, len - 1);
        break;
    case CTAP2_CMD_GET_ASSERTION:
        ESP_LOGI(TAG, "CBOR authenticatorGetAssertion");
        ctap2_get_assertion(cid, data + 1, len - 1);
        break;
    default: {
        // Honest "not yet implemented" rather than a fabricated success.
        ESP_LOGW(TAG, "CBOR cmd 0x%02x not implemented", cmd);
        uint8_t status = CTAP1_ERR_INVALID_CMD;
        ctaphid_send(cid, CTAPHID_CBOR, &status, 1);
        break;
    }
    }
}

// ---- Completed-message dispatch ---------------------------------------------
static void ctaphid_dispatch(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    switch (cmd) {
    case CTAPHID_INIT: {
        // Reply on the requesting channel with a freshly allocated CID.
        uint32_t new_cid = (cid == CTAPHID_BROADCAST_CID) ? next_cid++ : cid;
        uint8_t resp[17];
        memcpy(resp, data, CTAPHID_INIT_NONCE_LEN);       // echo the 8-byte nonce
        put_be32(&resp[8], new_cid);
        resp[12] = 2;                                     // CTAPHID protocol version
        resp[13] = 1;                                     // device version major
        resp[14] = 0;                                     // minor
        resp[15] = 0;                                     // build
        resp[16] = CAP_CBOR | CAP_WINK;                   // capabilities
        ESP_LOGI(TAG, "INIT -> cid 0x%08lx", (unsigned long)new_cid);
        ui_note_ctap("init");
        ctaphid_send(cid, CTAPHID_INIT, resp, sizeof(resp));
        break;
    }
    case CTAPHID_PING:
        ui_note_ctap("ping");
        ctaphid_send(cid, CTAPHID_PING, data, len);       // echo
        break;
    case CTAPHID_CBOR:
        ctap2_dispatch(cid, data, len);
        break;
    case CTAPHID_MSG: {
        // U2F/CTAP1 APDU path — not supported by the mule; return SW 0x6D00
        // (INS not supported) so hosts fall back cleanly instead of hanging.
        uint8_t sw[2] = { 0x6D, 0x00 };
        ctaphid_send(cid, CTAPHID_MSG, sw, sizeof(sw));
        break;
    }
    case CTAPHID_CANCEL:
        // Nothing long-running to cancel in the mule; acknowledge silently.
        break;
    case CTAPHID_WINK:
    case CTAPHID_LOCK:
        ctaphid_send(cid, cmd, NULL, 0);
        break;
    default:
        ctaphid_error(cid, ERR_INVALID_CMD);
        break;
    }
}

// ---- Packet ingest -----------------------------------------------------------
void ctaphid_rx_packet(const uint8_t *pkt)
{
    uint32_t cid = be32(pkt);
    bool is_init = (pkt[4] & 0x80) != 0;

    if (is_init) {
        uint8_t  cmd  = pkt[4] & 0x7F;
        uint16_t bcnt = ((uint16_t)pkt[5] << 8) | pkt[6];

        if (bcnt > CTAPHID_MAX_PAYLOAD) { ctaphid_error(cid, ERR_INVALID_LEN); return; }

        // A new init packet on a different channel while one is mid-assembly is
        // a busy condition (single-transaction mule).
        if (rx.active && cid != rx.cid) { ctaphid_error(cid, ERR_CHANNEL_BUSY); return; }

        uint16_t n = bcnt < 57 ? bcnt : 57;
        memcpy(rx.buf, &pkt[7], n);
        if (n >= bcnt) {
            ctaphid_dispatch(cid, cmd, rx.buf, bcnt);     // complete in one packet
            rx.active = false;
        } else {
            rx.active = true; rx.cid = cid; rx.cmd = cmd;
            rx.bcnt = bcnt; rx.off = n; rx.seq = 0;
        }
        return;
    }

    // Continuation packet.
    uint8_t seq = pkt[4];
    if (!rx.active || cid != rx.cid) return;              // stray continuation
    if (seq != rx.seq) { rx.active = false; ctaphid_error(cid, ERR_INVALID_SEQ); return; }

    uint16_t remaining = rx.bcnt - rx.off;
    uint16_t n = remaining < 59 ? remaining : 59;
    memcpy(&rx.buf[rx.off], &pkt[5], n);
    rx.off += n; rx.seq++;
    if (rx.off >= rx.bcnt) {
        ctaphid_dispatch(rx.cid, rx.cmd, rx.buf, rx.bcnt);
        rx.active = false;
    }
}
