#!/usr/bin/env bash
# Flash the moment a ROM download-mode port appears, and keep retrying across
# flickers. Use when the software escape hatch can't run: put the board in
# download mode physically (unplug, replug WHILE HOLDING BOOT ~2 s, then RELEASE
# BOOT so the USB stabilizes while download mode stays latched).
#
# The port on this dev unit can appear and vanish within a couple of seconds, so
# this grabs it fast (short settle) and, if esptool loses the race, waits for the
# port to reappear and tries again — for up to 15 minutes total.
set -uo pipefail
cd "$(dirname "$0")"
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
cd build

DEADLINE=$(( $(date +%s) + 10800 ))
echo "==> watching for a download-mode port (up to 3 h); flashing on sight ..."
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  if [ -z "$P" ]; then sleep 0.3; continue; fi
  echo "==> download port $P — flashing"
  sleep 1   # brief settle so macOS can configure the port (but short enough to beat a flicker)
  if python -m esptool --chip esp32s3 -p "$P" -b 460800 --before no_reset --after hard_reset \
       write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
       0x0 bootloader/bootloader.bin 0x10000 corekeys_mule.bin 0x8000 partition_table/partition-table.bin; then
    echo "==> FLASHED OK; board hard-reset into the new firmware"
    exit 0
  fi
  echo "==> attempt failed (port likely flickered); waiting for it to reappear ..."
  sleep 1
done
echo "==> watch window elapsed with no successful flash"
exit 1
