// Device Noise_KK responder + SSH co-authorization (docs/coauth-protocol.md §5–§6).
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "noise/protocol.h"
#include "ck_ed25519.h"
#include "ck_store.h"
#include "ckvp.h"
#include "session.h"
#include "corekeys.h"
#include "lcd.h"
#include "ui.h"

static const char *TAG = "session";

// The device's Noise X25519 static — generated once and stored in NVS on first
// boot (ck_store), NOT a shared constant. The authorized-daemon list (pinned via
// pairing) also lives in NVS. Only the SSH ed25519 credential remains fixed for
// now (the daemon still knows it out of band).
static uint8_t s_device_priv[32], s_device_pub[32];
static bool s_keys_ready;

static const uint8_t CK_SSH_SEED[32] = {
    0x63, 0x6f, 0x72, 0x65, 0x2d, 0x6b, 0x65, 0x79, 0x73, 0x2d, 0x6d, 0x75,
    0x6c, 0x65, 0x2d, 0x73, 0x73, 0x68, 0x2d, 0x73, 0x65, 0x65, 0x64, 0x2d,
    0x76, 0x31, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e, 0x2e,
};

#define BUTTON_GPIO      GPIO_NUM_0    // BOOT button
#define BUTTON_GPIO2     GPIO_NUM_14   // T-Display-S3 second button (enroll gesture)
#define BUTTON_WINDOW_MS 20000
#define REQ_ID_LEN       8

// Pairing (docs §4). Domain labels + sizes must match the daemon/protocol.
#define PAIR_NONCE_LEN   16
#define MACHINE_NAME_MAX 32
static const uint8_t L_COMMIT[] = "core-keys/pair/v1/commit";
static const uint8_t L_SAS[]    = "core-keys/pair/v1/sas";

enum { ST_IDLE = 0, ST_HANDSHAKING = 1, ST_TRANSPORT = 2 };
static int s_state = ST_IDLE;
static NoiseCipherState *s_send, *s_recv;
static uint16_t s_chan = 1;
static uint8_t s_ssh_pub[32];
static bool s_ssh_pub_ready, s_button_ready;

// Enroll / pairing (D side) state.
enum { PS_IDLE = 0, PS_GOT_INIT, PS_AWAIT_POP };
static bool s_enroll_armed;
static int s_pair_state;
static uint8_t s_pair_commit[32];
static uint8_t s_pair_machine[MACHINE_NAME_MAX];
static uint16_t s_pair_machine_len;
static uint8_t s_pair_d_nonce[PAIR_NONCE_LEN];
static uint8_t s_pair_h_pub[32];
static uint8_t s_pair_h_nonce[PAIR_NONCE_LEN];

int session_state(void) { return s_state; }
bool session_enroll_armed(void) { return s_enroll_armed; }

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
        if (ui_touch_approve_taken()) return true;       // on-screen APPROVE/MATCH tap
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

// Run the Noise_KK responder handshake against `remote_pub` (a candidate daemon
// static), using the device's NVS static. Returns 0 and leaves the session in
// ST_TRANSPORT on success; -1 (session reset) if this key is not the peer. The
// caller retries with the next authorized key, so a wrong key is not fatal.
static int do_handshake(const uint8_t *msg1, uint16_t len, const uint8_t *remote_pub)
{
    reset_session();
    if (!s_keys_ready) return -1;
    s_state = ST_HANDSHAKING;
    if (!s_ssh_pub_ready) { ck_ed25519_pubkey(CK_SSH_SEED, s_ssh_pub); s_ssh_pub_ready = true; }

    NoiseHandshakeState *hs = 0;
    if (noise_handshakestate_new_by_name(&hs, "Noise_KK_25519_ChaChaPoly_SHA256",
                                         NOISE_ROLE_RESPONDER) != NOISE_ERROR_NONE) { s_state = ST_IDLE; return -1; }
    noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(hs), s_device_priv, 32);
    noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(hs), remote_pub, 32);
    if (noise_handshakestate_start(hs) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return -1; }

    uint8_t inbuf[512], outbuf[512];
    NoiseBuffer mb;
    if (len > sizeof(inbuf)) { noise_handshakestate_free(hs); s_state = ST_IDLE; return -1; }
    memcpy(inbuf, msg1, len);
    noise_buffer_set_input(mb, inbuf, len);
    if (noise_handshakestate_read_message(hs, &mb, NULL) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return -1; }
    noise_buffer_set_output(mb, outbuf, sizeof(outbuf));
    if (noise_handshakestate_write_message(hs, &mb, NULL) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return -1; }
    ckvp_encode(CK_MT_NOISE_HS, s_chan, outbuf, mb.size, emit_vendor);
    if (noise_handshakestate_split(hs, &s_send, &s_recv) != NOISE_ERROR_NONE) { noise_handshakestate_free(hs); s_state = ST_IDLE; return -1; }
    noise_handshakestate_free(hs);
    s_state = ST_TRANSPORT;
    ESP_LOGI(TAG, "Noise session up");
    ui_note_ctap("session up");
    return 0;
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
    if (!ui_is_unlocked())
        return signresp_err(resp, r.req_id, r.req_id_len, "device locked");

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

// ---- FIDO2 co-authorization (device-initiated) -------------------------------
static uint8_t s_ca_req_id[REQ_ID_LEN];
static volatile int s_ca_verdict;   // CK_COAUTH_PENDING / APPROVE / DENY
static bool s_ca_active;

int session_coauth_begin(uint8_t op, const uint8_t *req_id,
                         const char *rp_id, uint16_t rp_len,
                         const uint8_t *client_hash,
                         const uint8_t *auth_data, uint16_t auth_len,
                         const uint8_t *cred_id, uint16_t cred_len)
{
    if (s_state != ST_TRANSPORT || !s_send) return -1;   // fail-closed: no desktop
    // Plaintext = tag 0x03 || CBOR array(6):
    //   [bytes req_id, uint op, text rp_id, bytes clientHash(32),
    //    bytes authData, bytes credId]. user_name/user_handle are absent, which
    // minicbor encodes as a shorter array (verified against the daemon wire).
    uint8_t buf[CKVP_MAX_MSG];
    int p = 0;
    buf[p++] = CK_RT_COAUTH_REQ;
    buf[p++] = 0x86;                                    // array(6)
    p += cbor_put_bytes(buf + p, req_id, REQ_ID_LEN);
    buf[p++] = (uint8_t)(op & 0x1F);                   // uint op (< 24)
    if (rp_len < 24) {
        buf[p++] = 0x60 | (uint8_t)rp_len;             // text rp_id
    } else {
        buf[p++] = 0x78; buf[p++] = (uint8_t)rp_len;
    }
    memcpy(buf + p, rp_id, rp_len); p += rp_len;
    p += cbor_put_bytes(buf + p, client_hash, 32);
    p += cbor_put_bytes(buf + p, auth_data, auth_len);
    p += cbor_put_bytes(buf + p, cred_id, cred_len);

    memcpy(s_ca_req_id, req_id, REQ_ID_LEN);
    s_ca_verdict = CK_COAUTH_PENDING;
    s_ca_active = true;
    send_encrypted(buf, (uint16_t)p);
    return 0;
}

int session_coauth_poll(void) { return s_ca_verdict; }
void session_coauth_end(void) { s_ca_active = false; }

// Decode a COAUTH_RESP (array(2/3): [bytes req_id, bool approve, opt text]) and
// latch the verdict if its req_id matches the outstanding request.
static void coauth_on_resp(const uint8_t *cbor, uint16_t len)
{
    if (!s_ca_active || len < 1) return;
    uint16_t p = 0;
    uint8_t ah = cbor[p++];
    if (ah != 0x82 && ah != 0x83) return;              // array(2) or array(3)
    const uint8_t *rid; uint16_t rid_len;
    if (cbor_str(cbor, len, &p, 2, &rid, &rid_len)) return;
    if (rid_len != REQ_ID_LEN || memcmp(rid, s_ca_req_id, REQ_ID_LEN) != 0) return;
    if (p >= len) return;
    uint8_t bv = cbor[p];
    if (bv == 0xF5)      s_ca_verdict = CK_COAUTH_APPROVE;
    else if (bv == 0xF4) s_ca_verdict = CK_COAUTH_DENY;
}

// ---- Site favicon push (daemon -> device, fire-and-forget) -------------------
// Raw binary body: rp_len(u8) || rp[rp_len] || w(u8) || h(u8) || pixels[w*h*2].
// Pixels are RGB565, little-endian u16 per pixel, row-major — stored verbatim.
// Untrusted input: validate strictly and drop silently on any mismatch.
static void icon_on_push(const uint8_t *body, uint16_t body_len)
{
    if (body_len < 3) return;
    uint8_t rp_len = body[0];
    if (body_len < (uint16_t)(1 + rp_len + 2)) return;
    uint8_t w = body[1 + rp_len];
    uint8_t h = body[2 + rp_len];
    if (!(w >= 1 && w <= 32 && h >= 1 && h <= 32)) return;
    const uint8_t *pixels = body + 3 + rp_len;
    uint16_t pix_len = (uint16_t)w * (uint16_t)h * 2u;
    if ((uint16_t)(3 + rp_len + pix_len) != body_len) return;
    ck_store_icon_set((const char *)(body + 1), rp_len, w, h, pixels);
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
    if (b.size < 1) return;

    // First plaintext byte is the record-type tag (lockstep with the daemon).
    uint8_t rt = buf[0];
    const uint8_t *body = buf + 1;
    uint16_t body_len = (uint16_t)(b.size - 1);
    if (rt == CK_RT_SIGN_REQ) {
        uint8_t out[128];
        out[0] = CK_RT_SIGN_RESP;
        uint16_t rlen = handle_signreq(body, body_len, out + 1);
        if (rlen) send_encrypted(out, (uint16_t)(rlen + 1));
    } else if (rt == CK_RT_COAUTH_RESP) {
        coauth_on_resp(body, body_len);
    } else if (rt == CK_RT_ICON_PUSH) {
        icon_on_push(body, body_len);
    }
    // Unknown tags: drop (no plaintext side effects, §5).
}

// ---- Pairing (D side, docs §4) ----------------------------------------------
// commit = SHA-256(L_COMMIT || h_pub || h_nonce || machine_name).
static void compute_commit(const uint8_t h_pub[32], const uint8_t h_nonce[PAIR_NONCE_LEN],
                           const uint8_t *machine, uint16_t machine_len, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, L_COMMIT, sizeof(L_COMMIT) - 1);
    mbedtls_sha256_update(&c, h_pub, 32);
    mbedtls_sha256_update(&c, h_nonce, PAIR_NONCE_LEN);
    mbedtls_sha256_update(&c, machine, machine_len);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

// SAS = SHA-256(L_SAS || d_pub || h_pub || d_nonce || h_nonce || machine_name),
// first 8 bytes big-endian mod 10^6.
static uint32_t compute_sas(const uint8_t d_pub[32], const uint8_t h_pub[32],
                            const uint8_t d_nonce[PAIR_NONCE_LEN], const uint8_t h_nonce[PAIR_NONCE_LEN],
                            const uint8_t *machine, uint16_t machine_len)
{
    uint8_t digest[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, L_SAS, sizeof(L_SAS) - 1);
    mbedtls_sha256_update(&c, d_pub, 32);
    mbedtls_sha256_update(&c, h_pub, 32);
    mbedtls_sha256_update(&c, d_nonce, PAIR_NONCE_LEN);
    mbedtls_sha256_update(&c, h_nonce, PAIR_NONCE_LEN);
    mbedtls_sha256_update(&c, machine, machine_len);
    mbedtls_sha256_finish(&c, digest);
    mbedtls_sha256_free(&c);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | digest[i];
    return (uint32_t)(v % 1000000ULL);
}

// PAIR_INIT { commit(32) || name_len(1) || machine_name } — reveal D's static
// and a fresh nonce in PAIR_RESP. Ignored unless enroll is physically armed.
static void on_pair_init(const uint8_t *data, uint16_t len)
{
    if (!s_enroll_armed) return;                 // reveal nothing until armed
    if (len < 33) return;
    uint8_t name_len = data[32];
    if (name_len > MACHINE_NAME_MAX || len != (uint16_t)(33 + name_len)) return;

    memcpy(s_pair_commit, data, 32);
    memcpy(s_pair_machine, data + 33, name_len);
    s_pair_machine_len = name_len;
    esp_fill_random(s_pair_d_nonce, PAIR_NONCE_LEN);

    uint8_t resp[32 + PAIR_NONCE_LEN];
    memcpy(resp, s_device_pub, 32);
    memcpy(resp + 32, s_pair_d_nonce, PAIR_NONCE_LEN);
    ckvp_encode(CK_MT_PAIR_RESP, s_chan, resp, sizeof(resp), emit_vendor);
    s_pair_state = PS_GOT_INIT;
    ESP_LOGI(TAG, "pairing: sent PAIR_RESP");
}

// PAIR_OPEN { h_pub(32) || h_nonce(16) } — check the commitment, show the SAS,
// wait for the button, and answer PAIR_CONFIRM. On confirm, expect the PoP.
static void on_pair_open(const uint8_t *data, uint16_t len)
{
    if (!s_enroll_armed || s_pair_state != PS_GOT_INIT) return;
    if (len != 32 + PAIR_NONCE_LEN) return;
    memcpy(s_pair_h_pub, data, 32);
    memcpy(s_pair_h_nonce, data + 32, PAIR_NONCE_LEN);

    uint8_t expect[32];
    compute_commit(s_pair_h_pub, s_pair_h_nonce, s_pair_machine, s_pair_machine_len, expect);
    uint8_t conf;
    if (memcmp(expect, s_pair_commit, 32) != 0) {   // interposer / corruption
        ESP_LOGW(TAG, "pairing: commit mismatch");
        ui_result("pair mismatch");
        s_pair_state = PS_IDLE;
        conf = 1;
        ckvp_encode(CK_MT_PAIR_CONFIRM, s_chan, &conf, 1, emit_vendor);
        return;
    }

    uint32_t sas = compute_sas(s_device_pub, s_pair_h_pub, s_pair_d_nonce, s_pair_h_nonce,
                               s_pair_machine, s_pair_machine_len);
    ui_pair_sas(sas, s_pair_machine, s_pair_machine_len);
    bool ok = button_wait(BUTTON_WINDOW_MS);
    conf = ok ? 0 : 1;
    ckvp_encode(CK_MT_PAIR_CONFIRM, s_chan, &conf, 1, emit_vendor);
    if (ok) {
        s_pair_state = PS_AWAIT_POP;
        ui_result("confirmed");
    } else {
        s_pair_state = PS_IDLE;
        ui_result("pair denied");
    }
}

void session_on_message(uint8_t msg_type, const uint8_t *data, uint16_t len, uint16_t chan)
{
    s_chan = chan;
    switch (msg_type) {
    case CK_MT_PAIR_INIT:
        on_pair_init(data, len);
        break;
    case CK_MT_PAIR_OPEN:
        on_pair_open(data, len);
        break;
    case CK_MT_NOISE_HS:
        if (s_enroll_armed && s_pair_state == PS_AWAIT_POP) {
            // Proof-of-possession: pin the daemon ONLY if the handshake completes.
            if (do_handshake(data, len, s_pair_h_pub) == 0) {
                ck_store_auth_add(s_pair_h_pub);
                ESP_LOGI(TAG, "pairing complete — daemon pinned");
                ui_result("PAIRED");
            } else {
                ui_result("pair PoP fail");
            }
            s_pair_state = PS_IDLE;
        } else {
            // Normal reconnect: try each pinned daemon until one is the peer.
            int n = ck_store_auth_count();
            for (int i = 0; i < n; i++) {
                uint8_t dpub[32];
                if (ck_store_auth_get(i, dpub) == 0 && do_handshake(data, len, dpub) == 0) break;
            }
        }
        break;
    case CK_MT_NOISE_MSG:
        do_transport(data, len);
        break;
    default:
        break;
    }
}

// ---- Boot-time init ----------------------------------------------------------
// Load (or first-boot generate) the device identity, and arm enroll if the
// enroll button (GPIO14) is held at boot. Call once from app_main.
void session_init(void)
{
    ck_store_init();
    if (ck_store_device_static(s_device_priv, s_device_pub) == 0) s_keys_ready = true;
    ck_ed25519_pubkey(CK_SSH_SEED, s_ssh_pub);
    s_ssh_pub_ready = true;
    ui_lock_boot();                  // boot locked iff a PIN is set

    button_init();
    if (gpio_get_level(BUTTON_GPIO2) == 0) {   // held low = pressed
        s_enroll_armed = true;
        ESP_LOGI(TAG, "ENROLL armed (GPIO14 held at boot)");
    }
    ESP_LOGI(TAG, "device identity ready; %d daemon(s) pinned; enroll=%d",
             ck_store_auth_count(), s_enroll_armed);
}
