#!/usr/bin/env bash
# Wait for the board to appear in ROM download mode, then flash it.
# Use this when the software escape hatch can't run (vendor interface wedged):
# put the board in download mode physically (unplug, then replug while holding
# BOOT for ~2 s), and this flashes the moment the download port shows up.
set -euo pipefail
cd "$(dirname "$0")"
source ~/esp/esp-idf/export.sh >/dev/null 2>&1

echo "==> waiting up to 1 h for a download-mode serial port ..."
PORT=""
for i in $(seq 1 3600); do
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  if [ -n "$P" ]; then PORT="$P"; echo "==> download port: $PORT (after ${i}s)"; break; fi
  sleep 1
done
[ -z "$PORT" ] && { echo "no download-mode port appeared in time"; exit 1; }

# Let the freshly-enumerated port settle — grabbing it immediately trips macOS
# pySerial with "Could not configure port: Device not configured".
echo "==> port up; letting it settle 3 s"
sleep 3

cd build
# Board is already in download mode (BOOT held on power-up), so do NOT pre-reset.
# Retry the connect a few times: macOS can still report the port busy briefly.
ok=0
for attempt in 1 2 3 4; do
  echo "==> flash attempt $attempt"
  if python -m esptool --chip esp32s3 -p "$PORT" -b 460800 --before no_reset --after hard_reset \
       write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
       0x0 bootloader/bootloader.bin 0x10000 corekeys_mule.bin 0x8000 partition_table/partition-table.bin; then
    ok=1; break
  fi
  echo "==> attempt $attempt failed; the port may still be settling — retrying in 2 s"
  sleep 2
  # The port name can change if the board re-enumerated between attempts.
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true); [ -n "$P" ] && PORT="$P"
done
[ "$ok" = 1 ] && echo "==> flashed; board hard-reset into the new firmware" \
             || { echo "==> all flash attempts failed"; exit 1; }
