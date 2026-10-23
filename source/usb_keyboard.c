#include "usb_keyboard.h"
#include <string.h>

#ifndef NXHID_USB_VID
// pid.codes test identity: private testing only, not a unique production allocation.
#define NXHID_USB_VID 0x1209
#endif
#ifndef NXHID_USB_PID
#define NXHID_USB_PID 0x0001
#endif

// Boot-compatible keyboard: modifiers, reserved byte, six key usages; LED output.
static const u8 report_descriptor[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07,
    0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08,
    0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x73,
    0x05,0x07, 0x19,0x00, 0x29,0x73, 0x81,0x00, 0xC0
};
static const u8 hid_descriptor[] = {
    9, USB_DT_HID, 0x11, 0x01, 0, 1, USB_DT_REPORT,
    sizeof(report_descriptor), 0
};
static UsbDsInterface *interface;
static UsbDsEndpoint *endpoint;
static bool initialized, was_ready;
// USB DMA buffers must remain page aligned and alive until session teardown.
static u8 control_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 input_buffer[0x1000] __attribute__((aligned(0x1000)));
static u8 last_report[8], idle_rate, protocol, leds;
static u64 last_sent;

enum {
    CONTROL_TIMEOUT_NS = 100000000,
    REPORT_TIMEOUT_MS = 250,
    EVENT_POLL_NS = 1000000,
};

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
static Result io_error(void) { return MAKERESULT(Module_Libnx, LibnxError_IoError); }

// Resetting an event that is not signaled fails with InvalidState; that is not a fault.
static Result clear_event(Event *event) {
    Result rc = eventClear(event);
    return rc == KERNELRESULT(InvalidState) ? 0 : rc;
}

void usb_keyboard_exit(void) {
    if (initialized) usbDsExit(); // Cancels transfers before buffers can be reused.
    initialized = was_ready = false;
    interface = NULL;
    endpoint = NULL;
}

Result usb_keyboard_init(void) {
    if (hosversionBefore(5, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;
    initialized = true;
#define TRY(expr) do { rc = (expr); if (R_FAILED(rc)) goto fail; } while (0)
    TRY(usbDsClearDeviceData());
    u8 language, manufacturer, product;
    const u16 langs[] = {0x0409};
    TRY(usbDsAddUsbLanguageStringDescriptor(&language, langs, 1));
    TRY(usbDsAddUsbStringDescriptor(&manufacturer, "NXHID"));
    TRY(usbDsAddUsbStringDescriptor(&product, "NXHID Wi-Fi Keyboard"));
    struct usb_device_descriptor device = {
        .bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200, .bMaxPacketSize0 = 64,
        .idVendor = NXHID_USB_VID, .idProduct = NXHID_USB_PID,
        .bcdDevice = 0x0100, .iManufacturer = manufacturer, .iProduct = product,
        .bNumConfigurations = 1
    };
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device));
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device));
    device.bcdUSB = 0x0300;
    device.bMaxPacketSize0 = 9;
    TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device));
    const u8 bos[] = {
        5,USB_DT_BOS,22,0,2,
        7,USB_DT_DEVICE_CAPABILITY,2,2,0,0,0,
        10,USB_DT_DEVICE_CAPABILITY,3,0,0x0e,0,1,0,0,0
    };
    TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    TRY(usbDsRegisterInterfaceEx(&interface, 0));
    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0, .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID, .bInterfaceSubClass = 1, .bInterfaceProtocol = 1
    };
    struct usb_endpoint_descriptor ep = {
        .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81, .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = 8, .bInterval = 10
    };
    struct usb_ss_endpoint_companion_descriptor companion = {
        .bLength = 6, .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION, .wBytesPerInterval = 8
    };
    for (UsbDeviceSpeed speed = UsbDeviceSpeed_Full; speed <= UsbDeviceSpeed_Super; speed++) {
        // Full speed: 10 ms; high/super: 2^(7-1) microframes = 8 ms.
        ep.bInterval = speed == UsbDeviceSpeed_Full ? 10 : 7;
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &intf, sizeof(intf)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, hid_descriptor, sizeof(hid_descriptor)));
        TRY(usbDsInterface_AppendConfigurationData(interface, speed, &ep, sizeof(ep)));
        if (speed == UsbDeviceSpeed_Super)
            TRY(usbDsInterface_AppendConfigurationData(interface, speed, &companion, sizeof(companion)));
    }
    TRY(usbDsInterface_RegisterEndpoint(interface, &endpoint, 0x81));
    TRY(usbDsInterface_EnableInterface(interface));
    memset(last_report, 0, sizeof(last_report));
    idle_rate = leds = 0;
    protocol = 1;
    last_sent = milliseconds();
    was_ready = false;
    TRY(usbDsEnable());
#undef TRY
    return 0;
fail:
    usb_keyboard_exit();
    return rc;
}

bool usb_keyboard_ready(void) {
    UsbState state;
    return initialized && R_SUCCEEDED(usbDsGetState(&state)) && state == UsbState_Configured;
}

uint8_t usb_keyboard_leds(void) { return leds; }

// Each control stage is bounded; any timeout requires session teardown by caller.
static Result control_stage(bool in, size_t size) {
    Event *event = in ? &interface->CtrlInCompletionEvent : &interface->CtrlOutCompletionEvent;
    Result rc = clear_event(event);
    if (R_FAILED(rc)) return rc;
    u32 id, transferred;
    rc = in ? usbDsInterface_CtrlInPostBufferAsync(interface, control_buffer, size, &id)
            : usbDsInterface_CtrlOutPostBufferAsync(interface, control_buffer, size, &id);
    if (R_FAILED(rc)) return rc;
    rc = eventWait(event, CONTROL_TIMEOUT_NS);
    if (R_FAILED(rc)) return rc;
    rc = clear_event(event);
    if (R_FAILED(rc)) return rc;
    UsbDsReportData reports;
    rc = in ? usbDsInterface_GetCtrlInReportData(interface, &reports)
            : usbDsInterface_GetCtrlOutReportData(interface, &reports);
    if (R_FAILED(rc)) return rc;
    rc = usbDsParseReportData(&reports, id, NULL, &transferred);
    if (R_FAILED(rc)) return rc;
    if (!in) armDCacheFlush(control_buffer, sizeof(control_buffer));
    return transferred == size ? 0 : io_error();
}

static Result control_reply(const void *data, size_t size, u16 requested) {
    if (!requested) return usbDsInterface_StallCtrl(interface);
    if (size > requested) size = requested;
    memcpy(control_buffer, data, size);
    Result rc = control_stage(true, size);
    return R_FAILED(rc) ? rc : control_stage(false, 0);
}

static Result service_setup(void) {
    Result rc = eventWait(&interface->SetupEvent, 0);
    if (rc == KERNELRESULT(TimedOut)) return 0;
    if (R_FAILED(rc)) return rc;
    rc = clear_event(&interface->SetupEvent);
    if (R_FAILED(rc)) return rc;
    struct usb_control_setup s;
    rc = usbDsInterface_GetSetupPacket(interface, &s, sizeof(s));
    if (R_FAILED(rc)) return rc;
    if (s.wIndex != interface->interface_index) return usbDsInterface_StallCtrl(interface);
    const u8 zero[2] = {0};
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_STATUS && s.wValue == 0 && s.wLength == 2)
        return control_reply(zero, 2, s.wLength);
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_INTERFACE && s.wValue == 0 && s.wLength == 1)
        return control_reply(zero, 1, s.wLength);
    if (s.bmRequestType == 0x01 && s.bRequest == USB_REQUEST_SET_INTERFACE && s.wValue == 0 && s.wLength == 0)
        return control_stage(true, 0);
    if (s.bmRequestType == 0x81 && s.bRequest == USB_REQUEST_GET_DESCRIPTOR && !(s.wValue & 0xff)) {
        if ((s.wValue >> 8) == USB_DT_REPORT)
            return control_reply(report_descriptor, sizeof(report_descriptor), s.wLength);
        if ((s.wValue >> 8) == USB_DT_HID)
            return control_reply(hid_descriptor, sizeof(hid_descriptor), s.wLength);
    }
    if (s.bmRequestType == 0xA1) {
        if (s.bRequest == 1 && s.wValue == 0x0100) // GET_REPORT input
            return control_reply(last_report, sizeof(last_report), s.wLength);
        if (s.bRequest == 1 && s.wValue == 0x0200) // GET_REPORT LEDs
            return control_reply(&leds, 1, s.wLength);
        if (s.bRequest == 2 && s.wValue == 0) // GET_IDLE
            return control_reply(&idle_rate, 1, s.wLength);
        if (s.bRequest == 3 && s.wValue == 0) // GET_PROTOCOL
            return control_reply(&protocol, 1, s.wLength);
    }
    if (s.bmRequestType == 0x21) {
        if (s.bRequest == 9 && s.wValue == 0x0200 && s.wLength == 1) { // SET_REPORT LEDs
            rc = control_stage(false, 1);
            if (R_FAILED(rc)) return rc;
            leds = control_buffer[0] & 0x1f;
            return control_stage(true, 0);
        }
        if (s.bRequest == 10 && !(s.wValue & 0xff) && s.wLength == 0) { // SET_IDLE
            idle_rate = s.wValue >> 8;
            return control_stage(true, 0);
        }
        if (s.bRequest == 11 && s.wValue <= 1 && s.wLength == 0) { // SET_PROTOCOL
            protocol = s.wValue;
            return control_stage(true, 0);
        }
    }
    return usbDsInterface_StallCtrl(interface);
}

Result usb_keyboard_write(const uint8_t report[8]) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    UsbState state;
    Result rc = usbDsGetState(&state);
    if (R_FAILED(rc)) return rc;
    if (state != UsbState_Configured) return io_error();
    memcpy(input_buffer, report, 8);
    rc = clear_event(&endpoint->CompletionEvent);
    if (R_FAILED(rc)) return rc;
    u32 id, transferred;
    rc = usbDsEndpoint_PostBufferAsync(endpoint, input_buffer, 8, &id);
    if (R_FAILED(rc)) return rc;
    u64 deadline = milliseconds() + REPORT_TIMEOUT_MS;
    for (;;) {
        rc = eventWait(&endpoint->CompletionEvent, EVENT_POLL_NS);
        if (R_SUCCEEDED(rc)) break;
        if (rc != KERNELRESULT(TimedOut)) return rc;
        rc = service_setup();
        if (R_FAILED(rc)) return rc;
        if (milliseconds() >= deadline) return KERNELRESULT(TimedOut);
    }
    rc = clear_event(&endpoint->CompletionEvent);
    if (R_FAILED(rc)) return rc;
    UsbDsReportData reports;
    rc = usbDsEndpoint_GetReportData(endpoint, &reports);
    if (R_FAILED(rc)) return rc;
    rc = usbDsParseReportData(&reports, id, NULL, &transferred);
    if (R_FAILED(rc)) return rc;
    if (transferred != 8) return io_error();
    memmove(last_report, report, 8);
    last_sent = milliseconds();
    return 0;
}

Result usb_keyboard_tick(void) {
    if (!initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = service_setup();
    if (R_FAILED(rc)) return rc;
    UsbState state;
    rc = usbDsGetState(&state);
    if (R_FAILED(rc)) return rc;
    bool ready = state == UsbState_Configured;
    if (!ready && was_ready) return io_error(); // Reset held state across unplug/suspend.
    if (ready && !was_ready) {
        was_ready = true;
        return usb_keyboard_write((const u8[8]){0});
    }
    if (ready && idle_rate && milliseconds() - last_sent >= (u64)idle_rate * 4)
        return usb_keyboard_write(last_report);
    return 0;
}
