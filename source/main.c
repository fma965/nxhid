#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "server.h"
#include "usb_keyboard.h"

static Result usb_fault;

static bool write_report(const uint8_t report[8]) {
    // Never reuse a DMA buffer following a failed transfer before teardown.
    if (R_FAILED(usb_fault)) return false;
    usb_fault = usb_keyboard_write(report);
    return R_SUCCEEDED(usb_fault);
}

static u64 milliseconds(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    bool sockets = false, network = false, usb = false;
    Server server = {.listener = -1, .client = -1};
    char message[160] = "Connect the Switch USB-C port directly to the target device.";
    Result rc = 0;

    if (appletGetAppletType() != AppletType_Application) {
        snprintf(message, sizeof(message), "Full application mode required. Hold R while launching a game, then open NXHID.");
        goto error;
    }
    rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    if (R_FAILED(rc)) goto error;
    rc = socketInitializeDefault();
    if (R_FAILED(rc)) goto error;
    sockets = true;
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) goto error;
    network = true;
    rc = usb_keyboard_init();
    if (R_FAILED(rc)) goto error;
    usb = true;
    if (!server_init(&server, NXHID_PORT, write_report, usb_keyboard_leds)) {
        snprintf(message, sizeof(message), "Cannot listen on TCP %d: %s", NXHID_PORT, strerror(errno));
        goto error;
    }
    appletSetAutoSleepDisabled(true);
    u64 next_draw = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_B) {
            server_drop(&server);
            snprintf(message, sizeof(message), "Sender disconnected. Waiting for a new connection.");
        }
        bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        if (R_SUCCEEDED(usb_fault)) usb_fault = usb_keyboard_tick();
        bool ready = R_SUCCEEDED(usb_fault) && usb_keyboard_ready();
        server_tick(&server, milliseconds(), ready && focused);
        if (R_FAILED(usb_fault)) {
            server_drop(&server);
            snprintf(message, sizeof(message), "USB reset (%08x). Reconnect the sender once USB is ready.", usb_fault);
            usb_keyboard_exit();
            usb = false;
            rc = usb_keyboard_init();
            if (R_FAILED(rc)) goto error;
            usb = true;
            usb_fault = 0;
        }
        u64 now = milliseconds();
        if (focused && now >= next_draw) {
            next_draw = now + 250;
            u32 ip = 0;
            char address[32] = "Wi-Fi not connected";
            if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip)
                inet_ntop(AF_INET, &ip, address, sizeof(address));
            printf("\x1b[2J\x1b[HNXHID - Wi-Fi to USB keyboard\n\n");
            printf("Switch IP:  %s\nTCP port:   %d\n\n", address, NXHID_PORT);
            printf("USB:        %s\nSender:     %s\nReports:    %lu\n", ready ? "Keyboard ready" : "Waiting for USB host", server.client >= 0 ? server.peer : "Waiting", server.reports);
            u8 leds = usb_keyboard_leds();
            printf("Host LEDs:  Num %s  Caps %s  Scroll %s\n\n", leds & 1 ? "on" : "off", leds & 2 ? "on" : "off", leds & 4 ? "on" : "off");
            printf("B: disconnect sender    +: exit\n\n%s\n\n", message);
            printf("Computer -> Wi-Fi -> Switch -> USB -> target device\n");
            consoleUpdate(NULL);
        }
        svcSleepThread(1000000);
    }
    goto cleanup;
error:
    server_close(&server);
    if (usb) { usb_keyboard_exit(); usb = false; }
    printf("\x1b[2J\x1b[HNXHID could not continue.\nResult: %08x\n%s\n\nPress + to exit.\n", rc, message);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
cleanup:
    server_close(&server);
    if (usb) usb_keyboard_exit();
    if (network) nifmExit();
    if (sockets) socketExit();
    appletSetAutoSleepDisabled(false);
    consoleExit(NULL);
    return 0;
}
