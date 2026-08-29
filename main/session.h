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
