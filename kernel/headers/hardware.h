#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>

void hardware_init(uint32_t multiboot_info_addr);
void hardware_print_info(void);

const char *hardware_get_cpu_vendor(void);
const char *hardware_get_cpu_brand(void);
int hardware_get_logical_cpus(void);
uint64_t hardware_get_ram_bytes(void);
const char *hardware_get_computer_name(void);

#endif