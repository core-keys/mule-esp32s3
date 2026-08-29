#!/usr/bin/env bash
# Flash the core-keys M1 mule. Uses the USB-Serial-JTAG port by default
# (the ESP32-S3 native download path). After flashing, the native USB
# switches to the TinyUSB composite HID device, so a *reflash* may need the
# board held in download mode (BOOT held during reset) if this port is gone.
set -euo pipefail
PORT="${1:-/dev/cu.usbmodem1401}"
cd "$(dirname "$0")"
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
echo "Flashing via $PORT ..."
idf.py -p "$PORT" flash
