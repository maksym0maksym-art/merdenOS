#ifndef XHCI_HID_H
#define XHCI_HID_H

#include <stdint.h>
#include "xhci.h"

/*
 * USB-миша (HID boot protocol) через xHCI: interrupt IN, полінг, без переривань.
 * Працює разом з будь-яким іншим джерелом (тачпад I2C, PS/2): mouse.c опитує обидва.
 */

/* Колбек для xhci_scan(): забирає boot-мишу (class 3 / subclass 1 / protocol 2). 1 — забрано. */
int  xhci_hid_probe(xhci_dev_t *dev, void *arg);

/* Забути всі миші (викликати перед повторним xhci_scan: він скидає контролер). */
void xhci_hid_forget_all(void);

/* Просканувати xHCI і підняти всі USB-миші. Повертає кількість мишей. */
int  xhci_mouse_init(void);

int  xhci_mouse_count(void);

/* Не блокує. 1 — отримано пакет руху. */
int  xhci_mouse_read(int *dx, int *dy, int *wheel, uint8_t *buttons);

#endif
