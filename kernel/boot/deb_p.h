#ifndef DEBP_H
#define DEBP_H

#include <stdint.h>

/* Рядки журналу завантаження (klog): тільки реальні події */
void boot_log_early(uint32_t magic);   /* multiboot, framebuffer, консоль, GDT */
void boot_log_hw(void);                /* CPU, пам'ять, IDE (після hardware_init + ide_init) */
void boot_log_usb(void);               /* флешки (після usbfs_autostart) */
void boot_log_done(void);              /* RTC + «завантаження завершено» */

#endif
