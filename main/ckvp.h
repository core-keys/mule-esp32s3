// CKVP framing over 64-byte vendor-HID reports — C port of protocol/src/ckvp.rs
// (docs/coauth-protocol.md §3). One reassembler tracks a single channel.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CKVP_MAX_MSG 2048
#define CKVP_REPORT  64

// msg_type values (0x00..0x3F).
enum {
    CK_MT_PAIR_INIT    = 0x01,
    CK_MT_PAIR_RESP    = 0x02,
    CK_MT_PAIR_OPEN    = 0x03,
    CK_MT_PAIR_CONFIRM = 0x04,
    CK_MT_NOISE_HS     = 0x10,
    CK_MT_NOISE_MSG    = 0x11,
};

typedef struct {
    bool     active;
    uint16_t chan;
    uint8_t  msg_type;
    uint16_t expected;
    uint16_t off;
    uint8_t  next_seq;
    uint8_t  buf[CKVP_MAX_MSG];
} ckvp_reasm_t;

void ckvp_reset(ckvp_reasm_t *r);

// Feed one 64-byte report. Returns 1 when a complete message is ready (payload
// in r->buf, length in *out_len, type in *out_type, channel in r->chan), 0 if
// more frames are needed, -1 on a framing error (frame dropped).
int ckvp_push(ckvp_reasm_t *r, const uint8_t report[CKVP_REPORT],
              uint8_t *out_type, uint16_t *out_len);

// Fragment a message into 64-byte reports, calling emit() for each.
void ckvp_encode(uint8_t msg_type, uint16_t chan, const uint8_t *payload,
                 uint16_t len, void (*emit)(const uint8_t report[CKVP_REPORT]));
