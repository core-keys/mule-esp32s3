// CTAP2 command handlers (authenticatorMakeCredential / authenticatorGetAssertion).
// Called from the CTAPHID CBOR dispatch; each owns its full response send so it
// can stream keepalives during the button wait.
#pragma once
#include <stdint.h>

void ctap2_make_credential(uint32_t cid, const uint8_t *req, uint16_t len);
void ctap2_get_assertion(uint32_t cid, const uint8_t *req, uint16_t len);
