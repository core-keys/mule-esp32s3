// Device Noise_KK responder + SSH co-authorization (docs/coauth-protocol.md §5–§6).
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "noise/protocol.h"
#include "ck_ed25519.h"
#include "ckvp.h"
#include "session.h"
#include "corekeys.h"
#include "lcd.h"

static const char *TAG = "session";

// ---- Fixed bring-up keys (NO pairing yet; match the daemon's gen_keys) -------
static const uint8_t CK_DEVICE_PRIV[32] = {
    0x10, 0xa1, 0x8e, 0x4f, 0x06, 0xcb, 0x47, 0x3c, 0x06, 0x4a, 0xb4, 0x1f,
    0x36, 0x80, 0xc7, 0x71, 0xa1, 0xf5, 0x43, 0x2a, 0xbe, 0xeb, 0xd1, 0x50,
    0x9a, 0x08, 0x5d, 0x84, 0x9d, 0xd4, 0xeb, 0x04,
};
static const uint8_t CK_DAEMON_PUB[32] = {
    0x4e, 0x6d, 0x2a, 0x36, 0x38, 0x32, 0xae, 0x81, 0x73, 0xfa, 0x7d, 0x1a,
    0xf2, 0x54, 0xac, 0x7d, 0x22, 0x49, 0xb7, 0xc7, 0xf3, 0xa2, 0x68, 0xc0,
    0x5c, 0xf3, 0xd6, 0x0e, 0x4f, 0x7f, 0x05, 0x31,
};
// Device SSH ed25519 credential seed (public is derived at boot).
static const uint8_t CK_SSH_SEED[32] = {
    0x63, 0x6f, 0x72, 0x65, 0x2d, 0x6b, 0x65, 0x79, 0x73, 0x2d, 0x6d, 0x75,
    0x6c, 0x65, 0x2d, 0x73, 0x73, 0x68, 0x2d, 0x73, 0x65, 0x65, 0x64, 0x2d,
    0x76, 0x31, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e,
};

#define BUTTON_GPIO      GPIO_NUM_0    // BOOT button
#define BUTTON_GPIO2     GPIO_NUM_14   // T-Display-S3 second button
#define BUTTON_WINDOW_MS 20000
#define REQ_ID_LEN       8

enum { ST_IDLE = 0, ST_HANDSHAKING = 1, ST_TRANSPORT = 2 };
static int s_state = ST_IDLE;
static NoiseCipherState *s_send, *s_recv;
static uint16_t s_chan = 1;
static uint8_t s_ssh_pub[32];
static bool s_ssh_pub_ready, s_button_ready;

int session_state(void) { return s_state; }

static void emit_vendor(const uint8_t report[CKVP_REPORT]) { ck_report_send(ITF_VENDOR, report); }

// ---- Minimal CBOR (only what the SIGN_REQ/SIGN_RESP records use) --------------
static int cbor_str(const uint8_t *b, uint16_t len, uint16_t *p, uint8_t major,
                    const uint8_t **out, uint16_t *outlen)
{
    if (*p >= len) return -1;
    uint8_t ib = b[*p];
    if ((ib >> 5) != major) return -1;
    uint8_t ai = ib & 0x1F;
    (*p)++;
    uint32_t l;
    if (ai < 24) l = ai;
    else if (ai == 24) { if (*p >= len) return -1; l = b[(*p)++]; }
    else if (ai == 25) { if (*p + 2 > len) return -1; l = ((uint32_t)b[*p] << 8) | b[*p + 1]; *p += 2; }
    else return -1;
    if ((uint32_t)*p + l > len) return -1;
    *out = &b[*p]; *outlen = (uint16_t)l; *p += l;
    return 0;
}
static int cbor_uint(const uint8_t *b, uint16_t len, uint16_t *p, uint32_t *out)
{
    if (*p >= len || (b[*p] >> 5) != 0) return -1;
    uint8_t ai = b[*p] & 0x1F; (*p)++;
    if (ai < 24) *out = ai;
    else if (ai == 24) { if (*p >= len) return -1; *out = b[(*p)++]; }
    else if (ai == 25) { if (*p + 2 > len) return -1; *out = ((uint32_t)b[*p] << 8) | b[*p + 1]; *p += 2; }
    else return -1;
    return 0;
}
static int cbor_bool(const uint8_t *b, uint16_t len, uint16_t *p, bool *out)
{
    if (*p >= len) return -1;
    if (b[*p] == 0xF4) { *out = false; (*p)++; return 0; }
    if (b[*p] == 0xF5) { *out = true;  (*p)++; return 0; }
    return -1;
}

typedef struct {
    const uint8_t *req_id; uint16_t req_id_len;
    uint32_t op;
    const uint8_t *tbs; uint16_t tbs_len;
    const uint8_t *hostkey; uint16_t hostkey_len;
    const char *nick; uint16_t nick_len;
    bool is_forwarding;
    const char *user; uint16_t user_len;
} signreq_t;

// SIGN_REQ is a CBOR array(7): [bytes, uint, bytes, bytes, text, bool, text].
static int signreq_decode(const uint8_t *b, uint16_t len, signreq_t *r)
{
    uint16_t p = 0;
    if (len < 1 || b[p++] != 0x87) return -1;   // array(7)
    if (cbor_str(b, len, &p, 2, &r->req_id, &r->req_id_len)) return -1;
    if (cbor_uint(b, len, &p, &r->op)) return -1;
    if (cbor_str(b, len, &p, 2, &r->tbs, &r->tbs_len)) return -1;
    if (cbor_str(b, len, &p, 2, &r->hostkey, &r->hostkey_len)) return -1;
    if (cbor_str(b, len, &p, 3, (const uint8_t **)&r->nick, &r->nick_len)) return -1;
    if (cbor_bool(b, len, &p, &r->is_forwarding)) return -1;
    if (cbor_str(b, len, &p, 3, (const uint8_t **)&r->user, &r->user_len)) return -1;
    return 0;
}
static int cbor_put_bytes(uint8_t *o, const uint8_t *d, uint16_t n)
{
    int p = 0;
    if (n < 24) o[p++] = 0x40 | n;
    else if (n < 256) { o[p++] = 0x58; o[p++] = n; }
    else { o[p++] = 0x59; o[p++] = n >> 8; o[p++] = n & 0xFF; }
    memcpy(o + p, d, n); return p + n;
}
// SIGN_RESP ok is a CBOR array(2): [bytes req_id, bytes sig] (trailing None error omitted).
static int signresp_ok(uint8_t *o, const uint8_t *req_id, uint16_t rlen, const uint8_t *sig, uint16_t slen)
{
    int p = 0; o[p++] = 0x82;
    p += cbor_put_bytes(o + p, req_id, rlen);
    p += cbor_put_bytes(o + p, sig, slen);
    return p;
}
// SIGN_RESP err is array(3): [bytes req_id, bytes(empty) sig, text error].
static int signresp_err(uint8_t *o, const uint8_t *req_id, uint16_t rlen, const char *err)
{
    int p = 0; o[p++] = 0x83;
    p += cbor_put_bytes(o + p, req_id, rlen);
    p += cbor_put_bytes(o + p, (const uint8_t *)"", 0);
    uint16_t el = (uint16_t)strlen(err);
    if (el < 24) o[p++] = 0x60 | el; else { o[p++] = 0x78; o[p++] = el; }
    memcpy(o + p, err, el); return p + el;
}

// ---- SSH wire strict-parse (bring-up subset) --------------------------------
static int ssh_str(const uint8_t *b, uint16_t len, uint16_t *p, const uint8_t **out, uint32_t *ol)
{
    if ((uint32_t)*p + 4 > len) return -1;
    uint32_t l = ((uint32_t)b[*p] << 24) | ((uint32_t)b[*p + 1] << 16) | ((uint32_t)b[*p + 2] << 8) | b[*p + 3];
    *p += 4;
    if ((uint32_t)*p + l > len) return -1;
    *out = &b[*p]; *ol = l; *p += l; return 0;
}
// Accept exactly a publickey / publickey-hostbound userauth blob or an SSHSIG,
// and extract the username for display. Returns 0 ok / -1 reject.
static int ssh_extract(const uint8_t *tbs, uint16_t len, const uint8_t **user, uint32_t *ulen)
{
    *user = (const uint8_t *)""; *ulen = 0;
    if (len >= 6 && memcmp(tbs, "SSHSIG", 6) == 0) return 0;  // git/ssh signature
    uint16_t p = 0;
    const uint8_t *s; uint32_t l;
    if (ssh_str(tbs, len, &p, &s, &l)) return -1;             // session id
    if (p >= len || tbs[p++] != 50) return -1;                // SSH_MSG_USERAUTH_REQUEST
    if (ssh_str(tbs, len, &p, user, ulen)) return -1;         // username
    if (ssh_str(tbs, len, &p, &s, &l)) return -1;             // service
    if (memcmp(s, "ssh-connection", l < 14 ? l : 14) != 0) return -1;
    if (ssh_str(tbs, len, &p, &s, &l)) return -1;             // method
    bool pk = (l == 9 && memcmp(s, "publickey", 9) == 0);
    bool hb = (l == 35 && memcmp(s, "publickey-hostbound-v00@openssh.com", 35) == 0);
    if (!pk && !hb) return -1;
    return 0;
}

// ---- Button ------------------------------------------------------------------
static void button_init(void)
{
    if (s_button_ready) return;
    gpio_config_t io = { .pin_bit_mask = (1ULL << BUTTON_GPIO) | (1ULL << BUTTON_GPIO2),
                         .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    s_button_ready = true;
}
static bool button_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == 0 || gpio_get_level(BUTTON_GPIO2) == 0;
}
static bool button_wait(uint32_t timeout_ms)
{
    button_init();
    for (uint32_t t = 0; t < timeout_ms; t += 20) {
        if (button_pressed()) {                          // pressed (active low)
            vTaskDelay(pdMS_TO_TICKS(30));               // debounce
            if (button_pressed()) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

// Live GPIO levels for STATUS diagnostics: bit0 = GPIO0, bit1 = GPIO14 (1=high).
uint8_t session_button_levels(void)
{
    button_init();
    return (gpio_get_level(BUTTON_GPIO) ? 1 : 0) | (gpio_get_level(BUTTON_GPIO2) ? 2 : 0);
}

// Public button primitives shared with the CTAP2 keepalive-covered gate (ctap2.c).
void ck_button_init(void)    { button_init(); }
bool ck_button_pressed(void) { return button_pressed(); }

// ---- Noise plumbing ----------------------------------------------------------
static void reset_session(void)
{
    if (s_send) { noise_cipherstate_free(s_send); s_send = 0; }
    if (s_recv) { noise_cipherstate_free(s_recv); s_recv = 0; }
    s_state = ST_IDLE;
}
static void send_encrypted(const uint8_t *plain, uint16_t plen)
{
    uint8_t buf[CKVP_MAX_MSG];
    if (plen > sizeof(buf) - 16) return;
    memcpy(buf, plain, plen);
    NoiseBuffer b;
    noise_buffer_set_inout(b, buf, plen, sizeof(buf));
    if (noise_cipherstate_encrypt(s_send, &b) != NOISE_ERROR_NONE) return;
    ckvp_encode(CK_MT_NOISE_MSG, s_chan, buf, b.size, emit_vendor);
}

static void do_handshake(const uint8_t *msg1, uint16_t len)
{
    reset_session();
    s_state = ST_HANDSHAKING;
    if (!s_ssh_pub_ready) { ck_ed25519_pubkey(CK_SSH_SEED, s_ssh_pub); s_ssh_pub_ready = true; }

    NoiseHandshakeState *hs = 0;
    if (noise_handshakestate_new_by_name(&hs, "Noise_KK_25519_ChaChaPoly_SHA256",
                                         NOISE_ROLE_RESPONDER) != NOISE_ERROR_NONE) { s_state = ST_IDLE; return; }
    noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(hs), CK_DEVICE_PRIV, 32);
    noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(hs), CK_DAEMON_PUB, 32);
    if (noise_handshakestate_start(hs) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return; }

    uint8_t inbuf[512], outbuf[512];
    NoiseBuffer mb;
    if (len > sizeof(inbuf)) { noise_handshakestate_free(hs); s_state = ST_IDLE; return; }
    memcpy(inbuf, msg1, len);
    noise_buffer_set_input(mb, inbuf, len);
    if (noise_handshakestate_read_message(hs, &mb, NULL) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return; }
    noise_buffer_set_output(mb, outbuf, sizeof(outbuf));
    if (noise_handshakestate_write_message(hs, &mb, NULL) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return; }
    ckvp_encode(CK_MT_NOISE_HS, s_chan, outbuf, mb.size, emit_vendor);
    if (noise_handshakestate_split(hs, &s_send, &s_recv) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return; }
    noise_handshakestate_free(hs);
    s_state = ST_TRANSPORT;
    ESP_LOGI(TAG, "Noise session up");
    ui_note_ctap("session up");
}

// Handle a decrypted SIGN_REQ: parse, show the approval, wait for the button,
// sign the exact tbs, and produce a SIGN_RESP into `resp` (returns its length).
static uint16_t handle_signreq(const uint8_t *plain, uint16_t plen, uint8_t *resp)
{
    signreq_t r;
    if (signreq_decode(plain, plen, &r) != 0 || r.req_id_len != REQ_ID_LEN)
        return 0;   // cannot even trust req_id — drop
    if (r.op != 0)  // 0 = ssh-userauth
        return signresp_err(resp, r.req_id, r.req_id_len, "unsupported op");

    const uint8_t *user; uint32_t ulen;
    if (ssh_extract(r.tbs, r.tbs_len, &user, &ulen) != 0) {
        ESP_LOGW(TAG, "strict SSH parse rejected");
        return signresp_err(resp, r.req_id, r.req_id_len, "bad blob");
    }

    // Build "user@nick" for the approval screen.
    char who[40];
    int un = ulen < 20 ? (int)ulen : 20;
    int nn = r.nick_len < 16 ? (int)r.nick_len : 16;
    snprintf(who, sizeof(who), "%.*s@%.*s", un, (const char *)user, nn ? nn : 1, nn ? r.nick : "?");

    ui_approval(who, r.is_forwarding);
    bool approved = button_wait(BUTTON_WINDOW_MS);
    if (!approved) { ui_result("denied"); return signresp_err(resp, r.req_id, r.req_id_len, "no button"); }

    uint8_t sig[64];
    ck_ed25519_sign(r.tbs, r.tbs_len, CK_SSH_SEED, s_ssh_pub, sig);
    ui_result("signed");
    return signresp_ok(resp, r.req_id, r.req_id_len, sig, 64);
}

static void do_transport(const uint8_t *data, uint16_t len)
{
    if (s_state != ST_TRANSPORT || !s_recv || !s_send) return;
    uint8_t buf[CKVP_MAX_MSG];
    if (len > sizeof(buf)) return;
    memcpy(buf, data, len);
    NoiseBuffer b;
    noise_buffer_set_inout(b, buf, len, sizeof(buf));
    if (noise_cipherstate_decrypt(s_recv, &b) != NOISE_ERROR_NONE) return;  // droppable (§5)

    uint8_t resp[128];
    uint16_t rlen = handle_signreq(buf, b.size, resp);
    if (rlen) send_encrypted(resp, rlen);
}

void session_on_message(uint8_t msg_type, const uint8_t *data, uint16_t len, uint16_t chan)
{
    s_chan = chan;
    if (msg_type == CK_MT_NOISE_HS) do_handshake(data, len);
    else if (msg_type == CK_MT_NOISE_MSG) do_transport(data, len);
}
