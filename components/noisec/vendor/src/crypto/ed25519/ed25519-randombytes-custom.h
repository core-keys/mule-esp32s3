/* core-keys: on-device Ed25519 is sign/verify only (the seed is provisioned,
   not generated on-device), so randombytes is never called. Stub it to avoid
   pulling in OpenSSL. Selected via -DED25519_CUSTOMRANDOM=1. */
void ED25519_FN(ed25519_randombytes_unsafe) (void *p, size_t len)
{
    (void)p;
    (void)len;
}
