// CTAPHID transport primitives shared with the CTAP2 command layer (ctap2.c).
#pragma once
#include <stdint.h>

#define CTAPHID_CBOR      0x10
#define CTAPHID_KEEPALIVE 0x3B

// CTAPHID keepalive status bytes.
#define CTAPHID_STATUS_PROCESSING 0x01
#define CTAPHID_STATUS_UPNEEDED   0x02

// Frame `data` as a CTAPHID message and send it on the CTAP interface.
void ctaphid_send(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len);

// Send a one-byte CTAPHID_KEEPALIVE — call this at <=100 ms cadence while a CBOR
// transaction waits on the button (or, later, on the daemon), or hosts abort.
void ctaphid_keepalive(uint32_t cid, uint8_t status);
