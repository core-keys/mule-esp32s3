#!/usr/bin/env bash
# Verify the mule enumerated as the composite core-keys device and that the
# host recognises the CTAP interface. Run after flashing + a board reset.
set -uo pipefail
echo "=== USB: core-keys device present? ==="
ioreg -p IOUSB -l -w0 | grep -iE '"USB Product Name" = "core-keys' && echo "OK: product string found" || echo "NOT FOUND"
echo
echo "=== fido2-token (libfido2) sees an authenticator? ==="
if command -v fido2-token >/dev/null 2>&1; then
  fido2-token -L
  for d in $(fido2-token -L | cut -d: -f1); do echo "--- $d ---"; fido2-token -I "$d"; done
else
  echo "fido2-token not installed (brew install libfido2) — skipping"
fi
