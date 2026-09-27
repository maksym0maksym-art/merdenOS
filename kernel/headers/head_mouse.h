#ifndef HID_MOUSE_H
#define HID_MOUSE_H

#include <stdint.h>
#include "uhci.h"
#include "usb.h"

typedef struct {
    uint8_t buttons;
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;
} mouse_report_t;

typedef struct {
    uhci_controller_t hc;
    usb_device_t dev;
    uint8_t endpoint_addr;
    uint16_t max_packet;
    uint8_t interface_num;
    uint8_t toggle;
} hid_mouse_t;

int hid_mouse_init(hid_mouse_t *m);

int hid_mouse_poll(hid_mouse_t *m, mouse_report_t *out);

#endif