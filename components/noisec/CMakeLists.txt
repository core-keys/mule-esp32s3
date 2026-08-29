# noise-c, pruned to Noise_KK_25519_ChaChaPoly_SHA256 for the core-keys device.
# See README.md for the exact upstream source and the patches applied.
set(V vendor)
idf_component_register(
    SRCS
        "${V}/src/protocol/cipherstate.c"
        "${V}/src/protocol/dhstate.c"
        "${V}/src/protocol/errors.c"
        "${V}/src/protocol/handshakestate.c"
        "${V}/src/protocol/hashstate.c"
        "${V}/src/protocol/internal.c"
        "${V}/src/protocol/names.c"
        "${V}/src/protocol/patterns.c"
        "${V}/src/protocol/randstate.c"
        "${V}/src/protocol/symmetricstate.c"
        "${V}/src/protocol/util.c"
        "${V}/src/backend/ref/dh-curve25519.c"
        "${V}/src/backend/ref/cipher-chachapoly.c"
        "${V}/src/backend/ref/hash-sha256.c"
        "${V}/src/crypto/chacha/chacha.c"
        "${V}/src/crypto/sha2/sha256.c"
        "${V}/src/crypto/donna/poly1305-donna.c"
        "${V}/src/stubs.c"
        "${V}/src/crypto/ed25519/ed25519.c"
        "esp_rand.c"
        "ck_ed25519.c"
    # curve25519-donna{,-c64}.c are #included by dh-curve25519.c, not compiled here.
    # rand_os.c is replaced by esp_rand.c (esp_fill_random) — see README.
    INCLUDE_DIRS "${V}/include" "."
    PRIV_INCLUDE_DIRS "${V}/src/protocol" "${V}/src"
    REQUIRES esp_hw_support
)
# noise-c uses HAVE_PTHREAD to guard a mutex around one-time init; the device is
# single-threaded through this path, so disable it. ED25519_REFHASH selects the
# bundled SHA-512 for the Ed25519 signing key.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_PTHREAD=0 ED25519_REFHASH=1 ED25519_CUSTOMRANDOM=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
