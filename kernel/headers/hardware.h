#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>

void hardware_init(uint32_t multiboot_info_addr);
void hardware_print_info(void);

#endif