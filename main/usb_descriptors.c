// core-keys M1 mule — USB descriptors for a composite two-HID device:
//   ITF 0: CTAPHID (FIDO usage page 0xF1D0) — a standard authenticator to the host
//   ITF 1: vendor  (usage page 0xFF00)      — the desktop-daemon side channel
//
// The device deliberately enumerates as an ordinary FIDO security key on ITF 0,
// so browsers, webauthn.dll, libfido2 and OpenSSH sk-* need zero vendor software;
// everything split-key rides ITF 1. This mirrors the frozen v1 topology (DESIGN.md).
#include "tusb.h"
#include "corekeys.h"

// ---- Endpoint numbers (address bit 0x80 = IN) --------------------------------
#define EPNUM_CTAP_OUT     0x01
#define EPNUM_CTAP_IN      0x81
#define EPNUM_VENDOR_OUT   0x02
#define EPNUM_VENDOR_IN    0x82

// ---- HID report descriptors --------------------------------------------------
// CTAPHID: FIDO Alliance usage page (0xF1D0), 64-byte In/Out reports, no report id.
const uint8_t desc_hid_report_ctap[] = {
    0x06, 0xD0, 0xF1,        // Usage Page (FIDO Alliance)
    0x09, 0x01,              // Usage (CTAPHID)
    0xA1, 0x01,              //  Collection (Application)
    0x09, 0x20,              //   Usage (Input Report Data)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8)
    0x95, 0x40,              //   Report Count (64)
    0x81, 0x02,              //   Input (Data,Var,Abs)
    0x09, 0x21,              //   Usage (Output Report Data)
    0x15, 0x00,              //   Logical Minimum (0)
    0x26, 0xFF, 0x00,        //   Logical Maximum (255)
    0x75, 0x08,              //   Report Size (8)
    0x95, 0x40,              //   Report Count (64)
    0x91, 0x02,              //   Output (Data,Var,Abs)
    0xC0                     //  End Collection
};

// Vendor channel: usage page 0xFF00, same 64-byte In/Out shape.
const uint8_t desc_hid_report_vendor[] = {
    0x06, 0x00, 0xFF,        // Usage Page (Vendor-defined 0xFF00)
    0x09, 0x01,              // Usage (0x01)
    0xA1, 0x01,              //  Collection (Application)
    0x09, 0x20,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x81, 0x02,              //   Input (Data,Var,Abs)
    0x09, 0x21,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x91, 0x02,              //   Output (Data,Var,Abs)
    0xC0                     //  End Collection
};

// ---- Device descriptor -------------------------------------------------------
// VID 0x303A is Espressif's; PID 0x4001 is a local dev id for the mule. The final
// device gets its own VID/PID and a stable AAGUID (DESIGN.md, attestation section).
const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,   // per-interface
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// ---- Configuration descriptor ------------------------------------------------
#define CK_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_INOUT_DESC_LEN)

const uint8_t desc_configuration[] = {
    // config number, interface count, string index, total length, attribute, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CK_CONFIG_TOTAL_LEN, 0x00, 100),

    // ITF 0 — CTAPHID:  itf, string, protocol, report len, EP out, EP in, EP size, poll ms
    TUD_HID_INOUT_DESCRIPTOR(ITF_CTAP, 4, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report_ctap),
                             EPNUM_CTAP_OUT, EPNUM_CTAP_IN, CK_REPORT_SIZE, 1),

    // ITF 1 — vendor daemon channel
    TUD_HID_INOUT_DESCRIPTOR(ITF_VENDOR, 5, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report_vendor),
                             EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, CK_REPORT_SIZE, 5),
};

// ---- String descriptors (UTF-8; esp_tinyusb converts to UTF-16) --------------
const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: language id — English (0x0409)
    "core-keys",                 // 1: manufacturer
    "core-keys mule",            // 2: product
    "M1-0001",                   // 3: serial
    "core-keys CTAPHID",         // 4: ITF 0
    "core-keys vendor",          // 5: ITF 1
};
const int string_desc_arr_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

// ---- TinyUSB callbacks -------------------------------------------------------
// Return the report descriptor for the requested HID instance.
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return (instance == ITF_CTAP) ? desc_hid_report_ctap : desc_hid_report_vendor;
}

// GET_REPORT control request — no feature reports in the mule.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// Data on the interrupt OUT endpoint arrives here with report_type == 0. This
// runs in the TinyUSB task; hand the packet to the worker task and return at
// once — processing here would block the USB task that our sends depend on.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize)
{
    (void)report_id; (void)report_type;
    if (bufsize < CK_REPORT_SIZE) return;   // mule expects full 64-byte reports
    ck_usb_rx_enqueue(instance, buffer);
}
