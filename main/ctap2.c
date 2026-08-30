// CTAP2 authenticatorMakeCredential / authenticatorGetAssertion.
//
// Stateless P-256 credentials (ck_credid.c), packed self-attestation, button
// User Presence with CTAPHID keepalives. No daemon co-authorization yet — this
// is the button-only closed loop verifiable with libfido2's fido2-cred /
// fido2-assert; the getAssertion co-auth gate lands in a later step.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "corekeys.h"    // ck_rx_poll / vendor_rx_packet / ITF_*
#include "ctap2.h"
#include "ctaphid.h"
#include "ck_p256.h"
#include "ck_credid.h"
#include "session.h"     // ck_button_init / ck_button_pressed / co-auth
#include "lcd.h"
#include "ui.h"

static const char *TAG = "ctap2";

// ---- CTAP2 status codes ------------------------------------------------------
#define CTAP2_OK                        0x00
#define CTAP1_ERR_INVALID_LENGTH        0x03
#define CTAP2_ERR_INVALID_CBOR          0x12
#define CTAP2_ERR_MISSING_PARAMETER     0x14
#define CTAP2_ERR_CREDENTIAL_EXCLUDED   0x19
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_OPERATION_DENIED      0x27
#define CTAP2_ERR_UNSUPPORTED_OPTION    0x2B
#define CTAP2_ERR_INVALID_OPTION        0x2C
#define CTAP2_ERR_KEEPALIVE_CANCEL      0x2D
#define CTAP2_ERR_NO_CREDENTIALS        0x2E
#define CTAP2_ERR_USER_ACTION_TIMEOUT   0x2F

#define FIDO_BUTTON_WINDOW_MS 20000
#define KEEPALIVE_INTERVAL_MS 80
// Desktop co-auth fail-closed deadline. Kept small so the whole ceremony (this
// + the button window) stays under a browser's ~30 s WebAuthn timeout.
#define COAUTH_DEADLINE_MS 2500

// ================= CBOR reader (definite-length, order-independent) ===========
typedef struct { const uint8_t *b; uint16_t len; uint16_t p; } cbr_t;

// Read one CBOR head: major type + argument (length / small value). Advances p.
static int cbr_head(cbr_t *c, uint8_t *major, uint64_t *arg)
{
    if (c->p >= c->len) return -1;
    uint8_t ib = c->b[c->p++];
    *major = ib >> 5;
    uint8_t ai = ib & 0x1F;
    if (ai < 24) { *arg = ai; return 0; }
    if (ai == 24) { if (c->p + 1 > c->len) return -1; *arg = c->b[c->p++]; return 0; }
    if (ai == 25) { if (c->p + 2 > c->len) return -1;
        *arg = ((uint64_t)c->b[c->p] << 8) | c->b[c->p + 1]; c->p += 2; return 0; }
    if (ai == 26) { if (c->p + 4 > c->len) return -1;
        *arg = ((uint64_t)c->b[c->p] << 24) | ((uint64_t)c->b[c->p + 1] << 16) |
               ((uint64_t)c->b[c->p + 2] << 8) | c->b[c->p + 3]; c->p += 4; return 0; }
    return -1;   // 8-byte args and indefinite lengths are not expected in CTAP2
}

// Skip exactly one CBOR value (recursively) — used for unknown map values.
static int cbr_skip(cbr_t *c)
{
    uint8_t major; uint64_t arg;
    if (cbr_head(c, &major, &arg)) return -1;
    switch (major) {
    case 0: case 1: case 7: return 0;                 // int / negint / simple
    case 2: case 3:                                   // byte / text string
        if ((uint64_t)c->p + arg > c->len) return -1;
        c->p += (uint16_t)arg; return 0;
    case 4:                                           // array
        for (uint64_t i = 0; i < arg; i++) if (cbr_skip(c)) return -1;
        return 0;
    case 5:                                           // map
        for (uint64_t i = 0; i < arg * 2; i++) if (cbr_skip(c)) return -1;
        return 0;
    case 6: return cbr_skip(c);                        // tag: skip tagged item
    default: return -1;
    }
}

static int cbr_map(cbr_t *c, uint32_t *n)
{ uint8_t m; uint64_t a; if (cbr_head(c, &m, &a) || m != 5) return -1; *n = (uint32_t)a; return 0; }
static int cbr_array(cbr_t *c, uint32_t *n)
{ uint8_t m; uint64_t a; if (cbr_head(c, &m, &a) || m != 4) return -1; *n = (uint32_t)a; return 0; }
static int cbr_uint(cbr_t *c, uint64_t *v)
{ uint8_t m; if (cbr_head(c, &m, v) || m != 0) return -1; return 0; }
static int cbr_int(cbr_t *c, int64_t *v)            // major 0 (n) or 1 (-1-n)
{ uint8_t m; uint64_t a; if (cbr_head(c, &m, &a)) return -1;
  if (m == 0) *v = (int64_t)a; else if (m == 1) *v = -1 - (int64_t)a; else return -1; return 0; }
static int cbr_bool(cbr_t *c, bool *v)
{ uint8_t m; uint64_t a; if (cbr_head(c, &m, &a) || m != 7) return -1;
  if (a == 20) *v = false; else if (a == 21) *v = true; else return -1; return 0; }
// Read a string of the given major (2 = bstr, 3 = tstr) as an in-place slice.
static int cbr_str(cbr_t *c, uint8_t major, const uint8_t **out, uint16_t *outlen)
{ uint8_t m; uint64_t a; if (cbr_head(c, &m, &a) || m != major) return -1;
  if ((uint64_t)c->p + a > c->len) return -1;
  *out = &c->b[c->p]; *outlen = (uint16_t)a; c->p += (uint16_t)a; return 0; }

// ================= CBOR writer ================================================
typedef struct { uint8_t *b; uint16_t cap; uint16_t p; bool ovf; } cbw_t;

static void cw_raw(cbw_t *w, const uint8_t *d, uint16_t n)
{ if (w->p + n > w->cap) { w->ovf = true; return; } memcpy(w->b + w->p, d, n); w->p += n; }
static void cw_byte(cbw_t *w, uint8_t v)
{ if (w->p + 1 > w->cap) { w->ovf = true; return; } w->b[w->p++] = v; }
static void cw_map(cbw_t *w, uint8_t n)  { cw_byte(w, 0xA0 | n); }
static void cw_uint(cbw_t *w, uint8_t n) { cw_byte(w, n); }        // n < 24
static void cw_nint(cbw_t *w, uint8_t v) { cw_byte(w, 0x20 | (v - 1)); } // -v, small
static void cw_tstr(cbw_t *w, const char *s)
{ uint16_t n = (uint16_t)strlen(s); cw_byte(w, 0x60 | n); cw_raw(w, (const uint8_t *)s, n); }
static void cw_bstr_hdr(cbw_t *w, uint16_t n)
{ if (n < 24) cw_byte(w, 0x40 | n);
  else if (n < 256) { cw_byte(w, 0x58); cw_byte(w, (uint8_t)n); }
  else { cw_byte(w, 0x59); cw_byte(w, n >> 8); cw_byte(w, n & 0xFF); } }
static void cw_bstr(cbw_t *w, const uint8_t *d, uint16_t n)
{ cw_bstr_hdr(w, n); cw_raw(w, d, n); }

// The 77-byte COSE ES256 public key: A5 01 02 03 26 20 01 21 5820<X> 22 5820<Y>.
static void cw_cose_es256(cbw_t *w, const uint8_t x[32], const uint8_t y[32])
{
    static const uint8_t head[] = { 0xA5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01,
                                    0x21, 0x58, 0x20 };
    cw_raw(w, head, sizeof(head));
    cw_raw(w, x, 32);
    cw_byte(w, 0x22); cw_byte(w, 0x58); cw_byte(w, 0x20);
    cw_raw(w, y, 32);
}

// ================= Error / keepalive helpers =================================
static void ctap_status(uint32_t cid, uint8_t status)
{ ctaphid_send(cid, CTAPHID_CBOR, &status, 1); }

// Keepalive-covered button gate. Returns true once the button is held; false on
// timeout. Streams CTAPHID_KEEPALIVE(UP_NEEDED) so libfido2/browsers don't abort.
static bool button_gate(uint32_t cid, uint32_t timeout_ms)
{
    ck_button_init();
    uint32_t elapsed = 0, since_ka = KEEPALIVE_INTERVAL_MS;   // fire one immediately
    while (elapsed < timeout_ms) {
        if (ck_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(30));                    // debounce
            if (ck_button_pressed()) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        elapsed += 20; since_ka += 20;
        if (since_ka >= KEEPALIVE_INTERVAL_MS) {
            ctaphid_keepalive(cid, CTAPHID_STATUS_UPNEEDED);
            since_ka = 0;
        }
    }
    return false;
}

// A short, NUL-terminated RP label for the approval screen.
static void rp_label(char *dst, size_t cap, const uint8_t *rp_id, uint16_t rp_len)
{
    size_t n = rp_len < cap - 1 ? rp_len : cap - 1;
    memcpy(dst, rp_id, n);
    dst[n] = 0;
}

// ---- Inline rx pump for the co-auth wait ------------------------------------
// The getAssertion handler runs on the single worker task, but the COAUTH_RESP
// it is waiting for also arrives on that task's queue — so it MUST drain the
// queue itself (a blocking receive would deadlock and force a reflash).
enum { CA_APPROVE = 0, CA_DENY, CA_TIMEOUT, CA_CANCEL };

// Is `pkt` a CTAPHID_CANCEL (cmd 0x11) init frame on our channel?
static bool is_ctaphid_cancel(const uint8_t *pkt, uint32_t cid)
{
    uint32_t pcid = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                    ((uint32_t)pkt[2] << 8) | pkt[3];
    return pcid == cid && (pkt[4] & 0x80) && (pkt[4] & 0x7F) == 0x11;
}

// Wait for the desktop verdict, pumping vendor packets into the session (which
// latches the COAUTH_RESP), streaming PROCESSING keepalives, and honoring CANCEL.
static int coauth_wait(uint32_t cid, uint32_t timeout_ms)
{
    uint32_t elapsed = 0, since_ka = KEEPALIVE_INTERVAL_MS;
    while (elapsed < timeout_ms) {
        ck_rx_item_t item;
        if (ck_rx_poll(&item, 20)) {
            if (item.itf == ITF_VENDOR) {
                vendor_rx_packet(item.data);
                int v = session_coauth_poll();
                if (v == CK_COAUTH_APPROVE) return CA_APPROVE;
                if (v == CK_COAUTH_DENY) return CA_DENY;
            } else if (item.itf == ITF_CTAP && is_ctaphid_cancel(item.data, cid)) {
                return CA_CANCEL;
            }
        }
        elapsed += 20; since_ka += 20;
        if (since_ka >= KEEPALIVE_INTERVAL_MS) {
            ctaphid_keepalive(cid, CTAPHID_STATUS_PROCESSING);
            since_ka = 0;
        }
    }
    return CA_TIMEOUT;
}

// The button gate used after approval: keeps pumping (so a CANCEL still lands
// and the session stays serviced) and streams UP_NEEDED keepalives.
// Returns 1 = pressed, 0 = timeout, -1 = cancelled.
static int button_gate_pumped(uint32_t cid, uint32_t timeout_ms)
{
    ck_button_init();
    uint32_t elapsed = 0, since_ka = KEEPALIVE_INTERVAL_MS;
    while (elapsed < timeout_ms) {
        if (ck_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (ck_button_pressed()) return 1;
        }
        ck_rx_item_t item;
        if (ck_rx_poll(&item, 20)) {
            if (item.itf == ITF_VENDOR) vendor_rx_packet(item.data);
            else if (item.itf == ITF_CTAP && is_ctaphid_cancel(item.data, cid)) return -1;
        }
        elapsed += 20; since_ka += 20;
        if (since_ka >= KEEPALIVE_INTERVAL_MS) {
            ctaphid_keepalive(cid, CTAPHID_STATUS_UPNEEDED);
            since_ka = 0;
        }
    }
    return 0;
}

// ================= authenticatorMakeCredential (0x01) =========================
void ctap2_make_credential(uint32_t cid, const uint8_t *req, uint16_t len)
{
    ui_note_ctap("makeCred");
    cbr_t c = { req, len, 0 };
    uint32_t nkeys;
    if (cbr_map(&c, &nkeys)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }

    const uint8_t *client_hash = NULL; uint16_t client_hash_len = 0;
    const uint8_t *rp_id = NULL; uint16_t rp_len = 0;
    bool have_user = false, alg_ok = false;
    bool opt_rk = false, opt_uv = false;
    const uint8_t *exclude = NULL; uint16_t exclude_pos = 0, exclude_at = 0;

    for (uint32_t i = 0; i < nkeys; i++) {
        uint64_t key;
        if (cbr_uint(&c, &key)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
        switch (key) {
        case 0x01:  // clientDataHash
            if (cbr_str(&c, 2, &client_hash, &client_hash_len))
                { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        case 0x02: {  // rp: map with "id"
            uint32_t rn;
            if (cbr_map(&c, &rn)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            for (uint32_t j = 0; j < rn; j++) {
                const uint8_t *k; uint16_t kl;
                if (cbr_str(&c, 3, &k, &kl)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                if (kl == 2 && memcmp(k, "id", 2) == 0) {
                    if (cbr_str(&c, 3, &rp_id, &rp_len)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                } else if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            }
            break;
        }
        case 0x03: {  // user: only its presence matters to us
            uint32_t un;
            if (cbr_map(&c, &un)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            for (uint32_t j = 0; j < un; j++) {
                const uint8_t *k; uint16_t kl;
                if (cbr_str(&c, 3, &k, &kl) || cbr_skip(&c))
                    { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            }
            have_user = true;
            break;
        }
        case 0x04: {  // pubKeyCredParams: first {type:"public-key", alg:-7} wins
            uint32_t an;
            if (cbr_array(&c, &an)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            for (uint32_t j = 0; j < an; j++) {
                uint32_t mn;
                if (cbr_map(&c, &mn)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                bool is_pk = false; int64_t alg = 0; bool alg_seen = false;
                for (uint32_t m = 0; m < mn; m++) {
                    const uint8_t *k; uint16_t kl;
                    if (cbr_str(&c, 3, &k, &kl)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                    if (kl == 4 && memcmp(k, "type", 4) == 0) {
                        const uint8_t *tv; uint16_t tl;
                        if (cbr_str(&c, 3, &tv, &tl)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                        is_pk = (tl == 10 && memcmp(tv, "public-key", 10) == 0);
                    } else if (kl == 3 && memcmp(k, "alg", 3) == 0) {
                        if (cbr_int(&c, &alg)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                        alg_seen = true;
                    } else if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                }
                if (is_pk && alg_seen && alg == -7) alg_ok = true;
            }
            break;
        }
        case 0x05:  // excludeList: remember position, scanned after rp is known
            exclude = c.b; exclude_pos = c.p; exclude_at = 1;
            if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        case 0x07: {  // options
            uint32_t on;
            if (cbr_map(&c, &on)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            for (uint32_t j = 0; j < on; j++) {
                const uint8_t *k; uint16_t kl;
                if (cbr_str(&c, 3, &k, &kl)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                bool bv;
                if (kl == 2 && memcmp(k, "rk", 2) == 0) {
                    if (cbr_bool(&c, &bv)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                    opt_rk = bv;
                } else if (kl == 2 && memcmp(k, "uv", 2) == 0) {
                    if (cbr_bool(&c, &bv)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                    opt_uv = bv;
                } else if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            }
            break;
        }
        default:
            if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        }
    }

    // Required-parameter and option validation.
    if (!client_hash || !rp_id || !have_user)
        { ctap_status(cid, CTAP2_ERR_MISSING_PARAMETER); return; }
    if (client_hash_len != 32) { ctap_status(cid, CTAP1_ERR_INVALID_LENGTH); return; }
    if (opt_rk) { ctap_status(cid, CTAP2_ERR_UNSUPPORTED_OPTION); return; }
    if (opt_uv) { ctap_status(cid, CTAP2_ERR_INVALID_OPTION); return; }
    if (!alg_ok) { ctap_status(cid, CTAP2_ERR_UNSUPPORTED_ALGORITHM); return; }

    uint8_t rpid_hash[32];
    ck_rpid_hash(rp_id, rp_len, rpid_hash);

    // excludeList: a descriptor whose id unwraps for THIS rp is a duplicate.
    if (exclude_at) {
        cbr_t e = { exclude, len, exclude_pos };
        uint32_t en;
        if (cbr_array(&e, &en) == 0) {
            for (uint32_t j = 0; j < en; j++) {
                uint32_t mn;
                if (cbr_map(&e, &mn)) break;
                const uint8_t *id = NULL; uint16_t idl = 0;
                for (uint32_t m = 0; m < mn; m++) {
                    const uint8_t *k; uint16_t kl;
                    if (cbr_str(&e, 3, &k, &kl)) { id = NULL; break; }
                    if (kl == 2 && memcmp(k, "id", 2) == 0) {
                        if (cbr_str(&e, 2, &id, &idl)) { id = NULL; break; }
                    } else if (cbr_skip(&e)) { id = NULL; break; }
                }
                uint8_t d_tmp[32];
                if (id && ck_credid_unwrap(id, idl, rpid_hash, d_tmp) == 0) {
                    ctap_status(cid, CTAP2_ERR_CREDENTIAL_EXCLUDED); return;
                }
            }
        }
    }

    // Fresh keypair + wrapped credentialId. Cover the keygen latency with a
    // PROCESSING keepalive so the host doesn't see an over-long silence.
    ctaphid_keepalive(cid, CTAPHID_STATUS_PROCESSING);
    uint8_t d[32], x[32], y[32], cred_id[CK_CREDID_LEN];
    if (ck_p256_keygen(d, x, y) != 0) { ctap_status(cid, CTAP2_ERR_OPERATION_DENIED); return; }
    if (ck_credid_wrap(d, rpid_hash, cred_id) != 0) { ctap_status(cid, CTAP2_ERR_OPERATION_DENIED); return; }

    // authData (225 bytes): rpIdHash||flags 0x41||signCount 0||aaguid 0||credLen||credId||COSE.
    uint8_t authdata[256];
    cbw_t a = { authdata, sizeof(authdata), 0, false };
    cw_raw(&a, rpid_hash, 32);
    cw_byte(&a, 0x41);                                   // UP | AT
    cw_byte(&a, 0); cw_byte(&a, 0); cw_byte(&a, 0); cw_byte(&a, 0);  // signCount 0
    for (int i = 0; i < 16; i++) cw_byte(&a, 0);         // AAGUID = zeros
    cw_byte(&a, (uint8_t)(CK_CREDID_LEN >> 8));          // credIdLen BE
    cw_byte(&a, (uint8_t)(CK_CREDID_LEN & 0xFF));
    cw_raw(&a, cred_id, CK_CREDID_LEN);
    cw_cose_es256(&a, x, y);
    if (a.ovf) { ctap_status(cid, CTAP2_ERR_OPERATION_DENIED); return; }
    uint16_t authdata_len = a.p;

    // WYSIWYS: show the RP, wait for the button (keepalive-covered).
    char who[40];
    rp_label(who, sizeof(who), rp_id, rp_len);
    ui_approval(who, false);
    if (!button_gate(cid, FIDO_BUTTON_WINDOW_MS)) {
        ui_result("denied");
        ctap_status(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }

    // Packed SELF attestation: DER ECDSA over authData||clientDataHash by the new key.
    uint8_t tbs[sizeof(authdata) + 32];
    memcpy(tbs, authdata, authdata_len);
    memcpy(tbs + authdata_len, client_hash, 32);
    uint8_t der[72]; size_t der_len = 0;
    if (ck_p256_sign(d, tbs, authdata_len + 32, der, sizeof(der), &der_len) != 0) {
        ui_result("error");
        ctap_status(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }

    // Response: {0x01 fmt "packed", 0x02 authData, 0x03 attStmt {alg:-7, sig}}.
    static uint8_t resp[512];
    cbw_t w = { resp, sizeof(resp), 0, false };
    cw_byte(&w, CTAP2_OK);
    cw_map(&w, 3);
    cw_uint(&w, 0x01); cw_tstr(&w, "packed");
    cw_uint(&w, 0x02); cw_bstr(&w, authdata, authdata_len);
    cw_uint(&w, 0x03);
        cw_map(&w, 2);
        cw_tstr(&w, "alg"); cw_nint(&w, 7);                 // -7
        cw_tstr(&w, "sig"); cw_bstr(&w, der, (uint16_t)der_len);
    if (w.ovf) { ctap_status(cid, CTAP2_ERR_OPERATION_DENIED); return; }

    ui_result("registered");
    ctaphid_send(cid, CTAPHID_CBOR, resp, w.p);
    ESP_LOGI(TAG, "makeCredential OK (%u byte resp)", w.p);
}

// ================= authenticatorGetAssertion (0x02) ===========================
void ctap2_get_assertion(uint32_t cid, const uint8_t *req, uint16_t len)
{
    ui_note_ctap("getAssert");
    cbr_t c = { req, len, 0 };
    uint32_t nkeys;
    if (cbr_map(&c, &nkeys)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }

    const uint8_t *rp_id = NULL; uint16_t rp_len = 0;
    const uint8_t *client_hash = NULL; uint16_t client_hash_len = 0;
    const uint8_t *allow = NULL; uint16_t allow_pos = 0; bool have_allow = false;
    bool opt_uv = false;

    for (uint32_t i = 0; i < nkeys; i++) {
        uint64_t key;
        if (cbr_uint(&c, &key)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
        switch (key) {
        case 0x01:  // rpId
            if (cbr_str(&c, 3, &rp_id, &rp_len)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        case 0x02:  // clientDataHash
            if (cbr_str(&c, 2, &client_hash, &client_hash_len)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        case 0x03:  // allowList: remember, scan after rpId is known
            allow = c.b; allow_pos = c.p; have_allow = true;
            if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        case 0x05: {  // options
            uint32_t on;
            if (cbr_map(&c, &on)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            for (uint32_t j = 0; j < on; j++) {
                const uint8_t *k; uint16_t kl;
                if (cbr_str(&c, 3, &k, &kl)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                bool bv;
                if (kl == 2 && memcmp(k, "uv", 2) == 0) {
                    if (cbr_bool(&c, &bv)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
                    opt_uv = bv;
                } else if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            }
            break;
        }
        default:
            if (cbr_skip(&c)) { ctap_status(cid, CTAP2_ERR_INVALID_CBOR); return; }
            break;
        }
    }

    if (!rp_id || !client_hash) { ctap_status(cid, CTAP2_ERR_MISSING_PARAMETER); return; }
    if (client_hash_len != 32) { ctap_status(cid, CTAP1_ERR_INVALID_LENGTH); return; }
    if (opt_uv) { ctap_status(cid, CTAP2_ERR_INVALID_OPTION); return; }
    if (!have_allow) { ctap_status(cid, CTAP2_ERR_NO_CREDENTIALS); return; }  // non-discoverable

    uint8_t rpid_hash[32];
    ck_rpid_hash(rp_id, rp_len, rpid_hash);

    // Select the first allowList id that unwraps for this RP.
    uint8_t d[32], sel_cred[CK_CREDID_LEN]; uint16_t sel_len = 0; bool found = false;
    cbr_t al = { allow, len, allow_pos };
    uint32_t an;
    if (cbr_array(&al, &an) == 0) {
        for (uint32_t j = 0; j < an && !found; j++) {
            uint32_t mn;
            if (cbr_map(&al, &mn)) break;
            const uint8_t *id = NULL; uint16_t idl = 0;
            for (uint32_t m = 0; m < mn; m++) {
                const uint8_t *k; uint16_t kl;
                if (cbr_str(&al, 3, &k, &kl)) { id = NULL; break; }
                if (kl == 2 && memcmp(k, "id", 2) == 0) {
                    if (cbr_str(&al, 2, &id, &idl)) { id = NULL; break; }
                } else if (cbr_skip(&al)) { id = NULL; break; }
            }
            if (id && idl == CK_CREDID_LEN && ck_credid_unwrap(id, idl, rpid_hash, d) == 0) {
                memcpy(sel_cred, id, idl); sel_len = idl; found = true;
            }
        }
    }
    if (!found) { ctap_status(cid, CTAP2_ERR_NO_CREDENTIALS); return; }

    // authData (37 bytes): rpIdHash || flags 0x01 (UP) || signCount 0.
    uint8_t authdata[37];
    memcpy(authdata, rpid_hash, 32);
    authdata[32] = 0x01;
    authdata[33] = authdata[34] = authdata[35] = authdata[36] = 0;

    // Split-key gate: the desktop must co-authorize BEFORE any button (docs §7).
    // No live session = no paired desktop present -> deny, never a button.
    uint8_t req_id[8];
    esp_fill_random(req_id, sizeof(req_id));
    ctaphid_keepalive(cid, CTAPHID_STATUS_PROCESSING);
    if (session_coauth_begin(CK_OP_FIDO2_ASSERT, req_id, (const char *)rp_id, rp_len,
                             client_hash, authdata, sizeof(authdata),
                             sel_cred, sel_len) != 0) {
        ui_result("no desktop");
        ctap_status(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }
    int ca = coauth_wait(cid, COAUTH_DEADLINE_MS);
    session_coauth_end();
    if (ca == CA_CANCEL) { ctap_status(cid, CTAP2_ERR_KEEPALIVE_CANCEL); return; }
    if (ca != CA_APPROVE) {
        ui_result("denied");                          // desktop deny or timeout
        ctap_status(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }

    // Approved -> WYSIWYS button (still pumping so a CANCEL lands).
    char who[40];
    rp_label(who, sizeof(who), rp_id, rp_len);
    ui_approval(who, false);
    int bg = button_gate_pumped(cid, FIDO_BUTTON_WINDOW_MS);
    if (bg == -1) { ui_result("cancelled"); ctap_status(cid, CTAP2_ERR_KEEPALIVE_CANCEL); return; }
    if (bg == 0) { ui_result("timeout"); ctap_status(cid, CTAP2_ERR_USER_ACTION_TIMEOUT); return; }

    uint8_t tbs[37 + 32];
    memcpy(tbs, authdata, 37);
    memcpy(tbs + 37, client_hash, 32);
    uint8_t der[72]; size_t der_len = 0;
    if (ck_p256_sign(d, tbs, sizeof(tbs), der, sizeof(der), &der_len) != 0) {
        ui_result("error");
        ctap_status(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }

    // Response: {0x01 credential descriptor, 0x02 authData, 0x03 signature}.
    static uint8_t resp[256];
    cbw_t w = { resp, sizeof(resp), 0, false };
    cw_byte(&w, CTAP2_OK);
    cw_map(&w, 3);
    cw_uint(&w, 0x01);
        cw_map(&w, 2);
        cw_tstr(&w, "id");   cw_bstr(&w, sel_cred, sel_len);
        cw_tstr(&w, "type"); cw_tstr(&w, "public-key");
    cw_uint(&w, 0x02); cw_bstr(&w, authdata, sizeof(authdata));
    cw_uint(&w, 0x03); cw_bstr(&w, der, (uint16_t)der_len);
    if (w.ovf) { ctap_status(cid, CTAP2_ERR_OPERATION_DENIED); return; }

    ui_result("signed in");
    ctaphid_send(cid, CTAPHID_CBOR, resp, w.p);
    ESP_LOGI(TAG, "getAssertion OK (%u byte resp)", w.p);
}
