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
    uint8_t toggle;          /* лишено для сумісності; toggle веде UHCI.c */
    uint8_t interval;
    int8_t  port;
    uint8_t dead;            /* мишку висмикнули */
    uint16_t errors;
} hid_mouse_t;

/* Шукає USB-мишку (boot protocol) на всіх UHCI-контролерах і всіх портах.
 * 1 — знайдено і запущено, 0 — ні. */
int hid_mouse_init(hid_mouse_t *m);

/* НЕ блокує. 1 — прийшов новий звіт, 0 — нічого нового. */
int hid_mouse_poll(hid_mouse_t *m, mouse_report_t *out);

#endif