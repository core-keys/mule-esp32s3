// Device Noise_KK responder session over the vendor channel.
// Bring-up scope: complete the handshake with the daemon and echo encrypted
// records. SIGN_REQ decode + SSH parse + approval come next.
#pragma once
#include <stdint.h>

// Feed a complete CKVP message (from the vendor channel) into the session.
void session_on_message(uint8_t msg_type, const uint8_t *data, uint16_t len, uint16_t chan);

// Session state for the display: 0 = idle, 1 = handshaking, 2 = transport up.
int session_state(void);

// Live button levels for STATUS diagnostics: bit0 = GPIO0, bit1 = GPIO14 (1=high).
uint8_t session_button_levels(void);

// Button primitives (active-low, either GPIO0 or GPIO14) for the CTAP2 gate.
#include <stdbool.h>
void ck_button_init(void);
bool ck_button_pressed(void);

// ---- Record-type tag (first byte of every Noise plaintext) -------------------
// Keep in lockstep with the daemon's corekeys_protocol::consts::record_type.
#define CK_RT_SIGN_REQ    0x01   // daemon -> device (SSH)
#define CK_RT_SIGN_RESP   0x02   // device -> daemon (SSH)
#define CK_RT_COAUTH_REQ  0x03   // device -> daemon (FIDO2)
#define CK_RT_COAUTH_RESP 0x04   // daemon -> device (FIDO2)

// Op values inside a co-auth request (match records::Op).
#define CK_OP_FIDO2_ASSERT   1
#define CK_OP_FIDO2_MAKECRED 2

// ---- FIDO2 co-authorization (device-initiated, docs §7) ----------------------
// The getAssertion handler must reach the daemon and be APPROVED before it shows
// any button. The device sends a COAUTH_REQ and then pumps the rx queue inline
// (it cannot block — the COAUTH_RESP arrives on the same worker task).
enum { CK_COAUTH_PENDING = 0, CK_COAUTH_APPROVE = 1, CK_COAUTH_DENY = -1 };

// Encrypt + send a COAUTH_REQ over the session. Returns 0 on success, -1 if
// there is no live session (fail-closed: no paired desktop present).
int session_coauth_begin(uint8_t op, const uint8_t *req_id,
                         const char *rp_id, uint16_t rp_len,
                         const uint8_t *client_hash,
                         const uint8_t *auth_data, uint16_t auth_len,
                         const uint8_t *cred_id, uint16_t cred_len);

// The latched verdict for the outstanding request (CK_COAUTH_*), updated when a
// COAUTH_RESP for the matching req_id is decrypted.
int session_coauth_poll(void);

// Clear the outstanding co-auth request (call when done/aborted).
void session_coauth_end(void);
