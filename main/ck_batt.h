// Battery fuel gauge for the LilyGO T-Display-S3: reads the on-board 2:1 divider
// on GPIO4 (ADC1_CH3) and maps LiPo voltage to a 0..100% state of charge.
#pragma once
#include <stdbool.h>

// Bring up the ADC + calibration. Safe to call once at boot.
void ck_batt_init(void);

// Current charge, 0..100, or -1 if the ADC is unavailable / no reading.
// Pass charging=true while USB power is attached: the charger drives VBAT above
// the cell's resting voltage, so a tapered IR-drop bias is subtracted first.
int  ck_batt_pct(bool charging);
