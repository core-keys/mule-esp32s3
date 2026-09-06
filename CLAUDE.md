# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF C firmware for a LilyGO T-Display-S3 acting as the core-keys "protocol
mule": a composite USB device (CTAP HID usage page 0xF1D0 plus a vendor HID
channel on 0xFF00). Prototype, not the final signing core.

core-keys is a split-key hardware authenticator for SSH and FIDO2. Three pieces
ship together and must stay in lockstep, and they live in three repositories:

- [core-keys/mule-esp32s3](https://github.com/core-keys/mule-esp32s3) — this
  firmware.
- [core-keys/daemon](https://github.com/core-keys/daemon) — the Rust desktop
  daemon `corekeys-daemon`: ssh-agent front end, Noise session owner, FIDO2
  co-authorization policy, pairing ceremony.
- [core-keys/protocol](https://github.com/core-keys/protocol) —
  `corekeys-protocol`, a `no_std` Rust crate holding wire logic both sides need.

**This firmware does not link `corekeys-protocol`. It reimplements the same wire
in C**, so every wire change is a two-sided edit — and since the split those
sides are separate repositories with no atomic CI. A change to `main/ckvp.c`
framing or to the `CK_RT_*` defines in `main/session.h` is not done until the
matching change lands in core-keys/protocol, and usually core-keys/daemon too.

Two documents are the source of truth and outrank any code comment. They live
in [core-keys/spec](https://github.com/core-keys/spec):

- `DESIGN.md` — frozen decisions D1 to D4, architecture, honest threat claims,
  milestones M1 to M5, board pinout and flashing notes.
- `docs/coauth-protocol.md` — the section-numbered wire protocol. Code comments
  cite it as "docs §N"; keep those citations accurate when you edit.

The invariant everything serves (docs §0): the device signs only when it has an
authenticated in-session request from the paired desktop, over the exact bytes
to be signed, AND the user presses the button after reading honest context on
the display.

## Commands

### Firmware (ESP-IDF v5.5.1)

```sh
source ~/esp/esp-idf/export.sh
idf.py build
./reflash.sh                 # normal loop: vendor escape hatch -> ROM download -> flash
./flash.sh [port]            # first flash, while USB-Serial-JTAG still exists
./flash_from_download.sh     # when the escape hatch cannot run (physical BOOT hold)
./flash_when_back.sh         # after a physical unplug: waits for re-enumeration, then reflashes
./verify-enumeration.sh      # confirms the composite device and CTAP interface
```

Once the app runs, TinyUSB owns the native USB and the download port disappears,
which is why the escape hatch exists (magic `C0 DE B0 07` on the vendor
channel). After flashing, a physical power cycle is still the reliable way to
actually boot the new image; see the flashing notes in core-keys/spec's
`DESIGN.md`.

### Device UI preview (no hardware)

```sh
cc -I main tools/ui_preview.c main/ui_draw.c main/ui_screens.c main/fonts.c -lm -o /tmp/uip
/tmp/uip /tmp/ui_sheet.bmp
```

Renders every screen into one contact sheet. Per the tool's own comment this is
the regression gate before any flash: run it after touching `ui_draw.c`,
`ui_screens.c` or `fonts.c`.

### Generated assets (do not hand-edit)

- `python3 tools/gen_font.py ui` regenerates `main/fonts.{h,c}` (4 bpp
  anti-aliased faces from the vendored TTFs in `tools/fonts/`). Bare
  `python3 tools/gen_font.py` regenerates the legacy 1 bpp `main/font8x16.h`
  from a system TTF. Needs pillow.

`tools/reboot-to-download.py` is the standalone escape-hatch trigger;
core-keys/daemon has the same thing as `examples/reboot_download.rs`, reusing
the daemon's Rust hidapi.

There is no CI in the repo.

## Architecture

### One transport stack, implemented twice

1. **Reports.** 64-byte HID reports on the vendor interface (0xFF00),
   deliberately unprivileged unlike the FIDO page. C: the rx queue in `main.c`
   plus `ck_report_send`. Rust: the `ReportLink` trait in the daemon's
   `src/transport.rs`.
2. **CKVP framing** (docs §3). START/CONT reassembly into logical messages
   capped at 2048 B. `main/ckvp.c` and core-keys/protocol's `src/ckvp.rs` are
   two implementations of one format.
3. **Noise_KK_25519_ChaChaPoly_SHA256** with both statics pinned at pairing.
   The device is responder (vendored noise-c in `components/noisec`, driven from
   `main/session.c`); the daemon is initiator. A live session *is* the proof the
   paired desktop is present.

Inside a Noise transport message: one record-type tag byte, then a CBOR body —
the `CK_RT_*` defines in `main/session.h` here, `record.rs` in the daemon,
`records.rs` in core-keys/protocol. Adding or renumbering a record is a
both-sides edit.

### Two flows share one session, in opposite directions

- **SSH is daemon-initiated.** The daemon sends a `SIGN_REQ`; this device runs
  its strict three-shape blob parser, shows the destination, waits for the
  button, and returns `SIGN_RESP`. The daemon annotates the request with the
  `session-bind@openssh.com` hostkey and a parsed username, but **the device's
  parse is the authoritative one**.
- **FIDO2 is device-initiated.** `ctap2.c` getAssertion sends a `COAUTH_REQ`,
  the daemon's `fido_policy.rs` approves or denies, then the device shows the rp
  id and takes the button.

### Firmware task topology

`app_main` starts one worker task that drains a queue fed from the TinyUSB
callback, so USB callbacks never block on our sends. `ui.c` runs a separate UI
task that is the sole owner of the LCD framebuffer: all other code calls `ui_*`
functions that only mutate state, and the UI task renders and animates. That
keeps drawing off the worker so co-auth timing is unaffected. The reboot escape
hatch is matched inside `ck_usb_rx_enqueue` (the USB driver path) specifically
so it survives a wedged worker task.

Buttons: GPIO0 is BOOT (select/commit/approve), GPIO14 is advance/scroll and is
also the enroll-arming hold at boot. The capacitive panel (`ck_touch.c`) can
supply the approval tap via `ui_touch_approve_taken()`.

### Device-side state

`ck_store.c` over NVS holds the device X25519 static (generated on first boot,
not a shared constant), the pinned daemon list (max 8), the local unlock PIN,
and a display-only credential journal. The PIN is a device unlock, not CTAP2
clientPIN: `ui_is_unlocked()` is the gate every crypto operation must check.
FIDO2 credentials themselves are stateless, wrapped credential IDs from
`ck_credid.c`.

## Things that will bite you

- **No plaintext frame may change device state** (docs §3.1, §11). Only
  `PAIR_*` and `NOISE_HS` / `NOISE_MSG` exist as message types. The mule's
  plaintext reboot/status/loopback control path is a known critical
  vulnerability kept only for the reflash loop; do not extend it, and it must
  not exist in a shipping image.
- **The display is the security boundary, not the button.** The button approves
  *a* signature; only the display makes it *the* signature. SSH destination is
  verified (hostbound blob, or a verified `session-bind`), never asserted.
  "Destination unverified" and the FORWARDED banner are real states, not
  decoration.
- **Bring-up keys are duplicated by hand** in `main/session.c` and, in
  core-keys/daemon, `src/main.rs` and `examples/hw_*.rs` — a *different
  repository* since the split. They apply only until a real pairing writes the
  config. Regenerate with `cargo run --example gen_keys` in core-keys/daemon,
  which prints C and Rust literals for both sides; the C literals get pasted
  here.
- `CK_MK` in `ck_credid.c` is a fixed mule-only master secret so credentials
  survive a reflash. Product derives it from eFuse or flash-encrypted NVS; the
  provenance is still open in docs §10.
- The device's SSH credential public key is still device-fixed (`SSH_PUB`) even
  after pairing.
- **docs §10 is the list of deliberate gaps** (req_id dedup specifics, chan
  allocation, canonical CBOR rules, ENROLL_APPROVE format, the architecture-B
  wrap layer, clientPIN policy). Read it before "fixing" something that only
  looks missing.

## Commit conventions

Subjects name the milestone or component and state the verification level
honestly: "VERIFIED on hardware" versus "software-verified". The project relies
on that distinction to know what has actually run on the board, and the
milestone list in core-keys/spec's `DESIGN.md` is updated as work lands.
