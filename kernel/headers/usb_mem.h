#ifndef USB_MEM_H
#define USB_MEM_H

#include <stdint.h>

void  usb_mem_init(void);
void* usb_alloc(uint32_t size, uint32_t align);
uint32_t usb_virt_to_phys(void* virt);

#endif