#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if;
    uint32_t bar4;
    uint8_t irq_line;
    /* додано для xHCI (MMIO BAR0 може бути 64-бітним) */
    uint32_t bar0, bar1;
} pci_device_t;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* перший UHCI-контролер (залишено для сумісності) */
int pci_find_uhci(pci_device_t *out);

/* index-ий UHCI-контролер (0, 1, 2 ...). Повертає 0, коли їх більше немає.
 * На q35/ICH9 їх зазвичай три, тому мишка може бути не на першому. */
int pci_find_uhci_n(int index, pci_device_t *out);

/* Універсальний пошук за класом: index-ий пристрій з class/subclass/prog_if.
 * Для xHCI: class 0x0C, subclass 0x03, prog_if 0x30. Заповнює bar0/bar1/bar4. */
int pci_find_class_n(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     int index, pci_device_t *out);

#endif
