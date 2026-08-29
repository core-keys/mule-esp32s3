// noise-c RNG backend for ESP32: replace rand_os.c (which relies on
// getrandom/getentropy/urandom, unavailable on the device) with the hardware
// RNG. esp_fill_random draws from the ESP32-S3 TRNG (valid once RF/Wi-Fi or the
// bootloader entropy source is up, which it is by the time app_main runs).
#include <stddef.h>
#include "esp_random.h"

void noise_rand_bytes(void *bytes, size_t size)
{
    esp_fill_random(bytes, size);
}
