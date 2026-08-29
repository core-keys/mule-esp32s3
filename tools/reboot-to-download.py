#!/usr/bin/env python3
"""Ask the running core-keys mule to reboot into ROM serial-download mode.

Once the mule firmware owns the ESP32-S3 native USB (TinyUSB), the USB-Serial-JTAG
flashing port disappears. This sends a magic frame over the *vendor* HID channel
(usage page 0xFF00) — which macOS/Windows let unprivileged tools open, unlike the
FIDO page — so the board becomes flashable again without a physical BOOT press.
After running this, reflash with:  firmware/mule-esp32s3/flash.sh

Requires: pip install hidapi
"""
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("pip install hidapi   # then re-run")

VID, PID = 0x303A, 0x4001
MAGIC = bytes([0xC0, 0xDE, 0xB0, 0x07])


def main():
    # Find the vendor interface (usage page 0xFF00); the CTAP interface is 0xF1D0.
    target = None
    for d in hid.enumerate(VID, PID):
        if d.get("usage_page") == 0xFF00:
            target = d
            break
    if not target:
        sys.exit("core-keys vendor interface (0303A:4001, usage page 0xFF00) not found")

    h = hid.device()
    h.open_path(target["path"])
    # HID write: prepend report id 0x00, pad the 64-byte report.
    report = bytes([0x00]) + MAGIC + bytes(64 - len(MAGIC))
    h.write(report)
    time.sleep(0.2)
    h.close()
    print("Sent reboot-to-download. Board should re-enumerate as USB-Serial-JTAG;",
          "reflash with firmware/mule-esp32s3/flash.sh")


if __name__ == "__main__":
    main()
