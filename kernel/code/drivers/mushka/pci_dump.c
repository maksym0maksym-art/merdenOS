#include "../../../headers/pci_dump.h"
#include "../../../headers/PCI.h"
#include "../../../../standarts_lib/stdio.h"
#include <stdint.h>

static char *hex(char *d, uint32_t v, int digits) {
    static const char H[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) *d++ = H[(v >> (i * 4)) & 0xF];
    return d;
}

/* Друкує всі PCI-пристрої: bus:slot.func vendor:device class */
void pci_dump_all(void) {
    int col = 0, total = 0;
    char line[16];
    t_printf("[pci] devices (bb:ss.f vend:dev class):\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (func == 0) break; else continue; }
                uint32_t cls = pci_config_read32((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 8) >> 16;
                char *p = line;
                p = hex(p, (uint32_t)bus, 2);  *p++ = ':';
                p = hex(p, (uint32_t)slot, 2); *p++ = '.';
                p = hex(p, (uint32_t)func, 1); *p++ = ' ';
                *p = 0;
                char b[32]; char *q = b;
                q = hex(q, id & 0xFFFF, 4); *q++ = ':';
                q = hex(q, id >> 16, 4);    *q++ = ' ';
                q = hex(q, cls, 4);         *q = 0;
                t_printf("%s%s  ", line, b);
                total++;
                if (++col == 3) { t_printf("\n"); col = 0; }
            }
        }
    }
    if (col) t_printf("\n");
    t_printf("[pci] total=%d\n", total);
}