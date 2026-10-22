#pragma once
#include <switch.h>

Result usb_keyboard_init(void);
void usb_keyboard_exit(void);
Result usb_keyboard_tick(void);
Result usb_keyboard_write(const uint8_t report[8]);
bool usb_keyboard_ready(void);
uint8_t usb_keyboard_leds(void);
