#!/usr/bin/env bash
# Wait for the board to re-enumerate in app mode (core-keys mule), let it settle,
# then software-reflash it (vendor escape hatch -> download mode -> flash). Use
# after a physical unplug: just plug the board back in (no button) and this does
# the rest.
set -euo pipefail
cd "$(dirname "$0")"

echo "==> waiting up to 2 h for the board (core-keys mule) to return on USB ..."
for i in $(seq 1 7200); do
  if ioreg -p IOUSB -l -w 0 2>/dev/null | grep -qi 'core-keys mule'; then
    echo "==> board back after ~${i}s; letting the firmware settle (worker + USB)"
    sleep 6
    exec ./reflash.sh
  fi
  sleep 1
done
echo "board did not return in time"; exit 1
