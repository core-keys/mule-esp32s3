// Device Noise_KK responder session over the vendor channel.
// Bring-up scope: complete the handshake with the daemon and echo encrypted
// records. SIGN_REQ decode + SSH parse + approval come next.
#pragma once
#include <stdint.h>

// Feed a complete CKVP message (from the vendor channel) into the session.
void session_on_message(uint8_t msg_type, const uint8_t *data, uint16_t len, uint16_t chan);

// Session state for the display: 0 = idle, 1 = handshaking, 2 = transport up.
int session_state(void);
