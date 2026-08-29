#include <string.h>
#include "ckvp.h"

#define START_PAYLOAD 59 // REPORT - 5
#define CONT_PAYLOAD  61 // REPORT - 3

void ckvp_reset(ckvp_reasm_t *r)
{
    r->active = false;
    r->off = 0;
    r->next_seq = 0;
}

int ckvp_push(ckvp_reasm_t *r, const uint8_t report[CKVP_REPORT],
              uint8_t *out_type, uint16_t *out_len)
{
    uint16_t chan = ((uint16_t)report[1] << 8) | report[2];
    bool is_start = (report[0] & 0x80) != 0;

    if (is_start) {
        uint8_t ty = report[0] & 0x7F;
        uint16_t len = ((uint16_t)report[3] << 8) | report[4];
        if (len > CKVP_MAX_MSG) return -1;
        if (r->active && chan != r->chan) return -1; // busy: another channel

        r->active = true;
        r->chan = chan;
        r->msg_type = ty;
        r->expected = len;
        r->next_seq = 0;
        uint16_t n = len < START_PAYLOAD ? len : START_PAYLOAD;
        memcpy(r->buf, &report[5], n);
        r->off = n;
    } else {
        if (!r->active || chan != r->chan) return -1; // stray continuation
        uint8_t seq = report[0] & 0x7F;
        if (seq != r->next_seq) { ckvp_reset(r); return -1; }
        uint16_t remaining = r->expected - r->off;
        uint16_t n = remaining < CONT_PAYLOAD ? remaining : CONT_PAYLOAD;
        if (r->off + n > CKVP_MAX_MSG) { ckvp_reset(r); return -1; }
        memcpy(&r->buf[r->off], &report[3], n);
        r->off += n;
        r->next_seq = (r->next_seq + 1) & 0x7F;
    }

    if (r->off >= r->expected) {
        r->active = false;
        *out_type = r->msg_type;
        *out_len = r->expected;
        return 1;
    }
    return 0;
}

void ckvp_encode(uint8_t msg_type, uint16_t chan, const uint8_t *payload,
                 uint16_t len, void (*emit)(const uint8_t report[CKVP_REPORT]))
{
    uint8_t report[CKVP_REPORT];

    // START
    memset(report, 0, sizeof(report));
    report[0] = 0x80 | (msg_type & 0x7F);
    report[1] = (uint8_t)(chan >> 8);
    report[2] = (uint8_t)(chan & 0xFF);
    report[3] = (uint8_t)(len >> 8);
    report[4] = (uint8_t)(len & 0xFF);
    uint16_t n = len < START_PAYLOAD ? len : START_PAYLOAD;
    if (n) memcpy(&report[5], payload, n);
    emit(report);
    uint16_t off = n;

    // CONT
    uint8_t seq = 0;
    while (off < len) {
        memset(report, 0, sizeof(report));
        report[0] = seq & 0x7F;
        report[1] = (uint8_t)(chan >> 8);
        report[2] = (uint8_t)(chan & 0xFF);
        n = (len - off) < CONT_PAYLOAD ? (len - off) : CONT_PAYLOAD;
        memcpy(&report[3], &payload[off], n);
        emit(report);
        off += n;
        seq = (seq + 1) & 0x7F;
    }
}
