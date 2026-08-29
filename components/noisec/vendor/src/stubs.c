/* Stubs for algorithms not built into the core-keys KK-only subset.
   None are reachable: the algorithm dispatch switches keep only
   Curve25519 / ChaChaPoly / SHA256. */
typedef struct NoiseCipherState_s NoiseCipherState;
NoiseCipherState *noise_aesgcm_new_ref(void) { return 0; }
