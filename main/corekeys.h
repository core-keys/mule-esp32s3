// core-keys M1 protocol mule — shared declarations.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// USB HID instances (composite device, two HID interfaces).
#define ITF_CTAP    0   // TLC-1, FIDO usage page 0xF1D0 — the standard authenticator
#define ITF_VENDOR  1   // TLC-2, vendor usage page 0xFF00 — the desktop-daemon channel

// One 64-byte HID report as received from either interface's OUT endpoint.
#define CK_REPORT_SIZE 64

// Send a fully-formed 64-byte report on the given HID instance (blocks until
// the endpoint is ready). Defined in main.c; used by the CTAPHID layer.
// Safe to call ONLY from the worker task, never from a TinyUSB callback.
void ck_report_send(uint8_t itf, const uint8_t *report64);

// Enqueue a received 64-byte report for the worker task. Called from the
// TinyUSB set_report callback; returns immediately so the USB task never blocks.
void ck_usb_rx_enqueue(uint8_t itf, const uint8_t *pkt64);

// Worker-task entry points (run outside USB-callback context, so they may send).
void ctaphid_rx_packet(const uint8_t *pkt64);   // ITF_CTAP state machine
void vendor_rx_packet(const uint8_t *pkt64);    // ITF_VENDOR — M1 loopback
