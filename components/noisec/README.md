# noisec — noise-c pruned to Noise_KK for the core-keys device

The device's Noise_KK responder. This is [rweather/noise-c](https://github.com/rweather/noise-c)
(MIT), reduced to exactly the one Noise protocol the device speaks:
**`Noise_KK_25519_ChaChaPoly_SHA256`**, plus an ESP32 RNG backend.

## Why noise-c (not hand-rolled)

Rolling a Noise state machine by hand is novel security-critical code — exactly
what this project's threat model warns against. noise-c is a mature, spec-correct
implementation. Interop with the daemon's `snow` (Rust) is proven on the host by
`daemon/examples/noise_kk_interop.rs` (both transport directions), so the crypto
is validated before it reaches the hardware, where each fix would cost a flash +
power-cycle.

## Patches applied to upstream

1. **Algorithm dispatch pruned to KK.** The `new_by_id` switches in
   `dhstate.c` / `cipherstate.c` / `hashstate.c` keep only Curve25519 /
   ChaChaPoly / SHA256; the other cases (curve448, newhope, aes-gcm, blake2,
   sha512) are removed so their crypto need not be compiled.
2. **`dh-curve25519.c` keygen** uses `curve25519_donna(pub, priv, {9})` instead
   of ed25519-donna's `curved25519_scalarmult_basepoint`, dropping the ed25519
   dependency (unused: KK has no signatures).
3. **`stubs.c`** provides `noise_aesgcm_new_ref()` returning NULL — referenced by
   `internal.c`'s (unreachable) `noise_aesgcm_new()`.
4. **`esp_rand.c`** replaces upstream `rand_os.c`: `noise_rand_bytes` draws from
   the ESP32-S3 TRNG via `esp_fill_random`.

Sign support (`signstate.c`, `sign-ed25519.c`, the ed25519 crypto) is omitted
entirely — `handshakestate.c` does not reference it for KK.

## Regenerating

Clone upstream, apply the four patches above (the switch prunes are mechanical),
copy the file set in `CMakeLists.txt`. The host build recipe is in the
`daemon/examples/noise_kk_interop.rs` header and the project notes.
