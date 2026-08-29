#!/usr/bin/env bash
# Reflash the running mule without a physical BOOT press.
#
# The app firmware owns the native USB (TinyUSB), so the USB-Serial-JTAG port is
# gone and esptool's default RTS reset would just reboot the app (and clears the
# force-download bit). Instead: send the vendor-channel escape hatch to force ROM
# download mode, then flash with --before no_reset (the board is ALREADY in
# download mode) and --after hard_reset to boot the new image.
set -euo pipefail
cd "$(dirname "$0")"
source ~/esp/esp-idf/export.sh >/dev/null 2>&1

echo "==> forcing ROM download mode via vendor escape hatch"
/Users/felipe/core-keys/target/debug/examples/reboot_download || {
  echo "escape hatch failed — is the board enumerated as core-keys mule?"; exit 1; }

PORT=""
for i in $(seq 1 10); do
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  if [ -n "$P" ]; then PORT="$P"; break; fi
  sleep 1
done
[ -z "$PORT" ] && { echo "no download-mode serial port appeared"; exit 1; }
echo "==> download port: $PORT"

cd build
# Short settle then retry: the port needs a beat to be configurable, but a long
# wait risks it flickering away (flaky USB on this dev unit).
ok=0
for attempt in 1 2 3 4 5 6; do
  sleep 1
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true); [ -n "$P" ] && PORT="$P"
  echo "==> flash attempt $attempt on $PORT"
  if python -m esptool --chip esp32s3 -p "$PORT" -b 460800 --before no_reset --after hard_reset \
       write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
       0x0 bootloader/bootloader.bin 0x10000 corekeys_mule.bin 0x8000 partition_table/partition-table.bin; then
    ok=1; break
  fi
  echo "==> attempt $attempt failed; retrying"
done
[ "$ok" = 1 ] && echo "==> flashed; board hard-reset into the new firmware" \
             || { echo "==> all flash attempts failed"; exit 1; }
