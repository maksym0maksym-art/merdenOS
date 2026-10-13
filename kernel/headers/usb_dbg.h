#ifndef USB_DBG_H
#define USB_DBG_H

#include <stdint.h>

/*
 * Спільні дрібниці для нового USB-стеку (xHCI / Mass Storage / FAT32):
 *   - usb_printf   : мінімальний printf (%s %d %u %x %02x %08x %c %%), виводить у shell
 *   - USB_DBG      : те саме, але тільки коли `usb debug on`
 *   - таймери      : по TSC (відкалібрований по PIT), без переривань
 */

extern int usb_debug;                 /* 0 — тільки важливе, 1 — трасування всіх команд */

void usb_printf(const char *fmt, ...);

#define USB_DBG(...) do { if (usb_debug) usb_printf(__VA_ARGS__); } while (0)

/* ---- час ---- */
typedef uint64_t usb_deadline_t;

void           usb_time_init(void);               /* калібрування (можна викликати повторно) */
usb_deadline_t usb_deadline_ms(uint32_t ms);
int            usb_expired(usb_deadline_t d);     /* 1, коли час вийшов */
void           usb_delay_ms(uint32_t ms);
void           usb_delay_us(uint32_t us);

#endif
