// Capacitive touch for the T-Display-S3 Touch (CST816 / CST328 on I2C).
// Pins from LilyGO's config: SCL=17, SDA=18, INT=16, RST=21.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Bring up I2C + reset the controller, then probe for CST816 (0x15) and
// CST328 (0x1A). Returns the 7-bit address that answered, or 0 if none.
uint8_t ck_touch_init(void);

// The detected controller address (0 = none / not the Touch variant).
uint8_t ck_touch_addr(void);

// Read the current touch point. Returns true if a finger is down and fills
// x,y in DISPLAY coordinates (0..169, 0..319). gesture (if non-NULL) gets the
// controller's gesture code (CST816: 1 up,2 down,3 left,4 right,5 tap,12 long).
bool ck_touch_read(int *x, int *y, uint8_t *gesture);
