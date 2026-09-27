#include "../../../headers/PCI.h"
#include "../../../headers/ports.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)(
        (1u << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFC)
    );
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((val >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t addr = pci_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDRESS, addr);
    uint32_t old = inl(PCI_CONFIG_DATA);
    uint32_t shift = (offset & 2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t merged = (old & ~mask) | ((uint32_t)value << shift);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, merged);
}

int pci_find_uhci(pci_device_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read16((uint8_t)bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;

                uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                    out->bus = (uint8_t)bus;
                    out->slot = slot;
                    out->func = func;
                    out->vendor_id = vendor;
                    out->device_id = pci_config_read16((uint8_t)bus, slot, func, 0x02);
                    out->class_code = class_code;
                    out->subclass = subclass;
                    out->prog_if = prog_if;
                    out->bar4 = pci_config_read32((uint8_t)bus, slot, func, 0x20) & ~0x3u;
                    out->irq_line = (uint8_t)(pci_config_read32((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                    return 1;
                }
            }
        }
    }
    return 0;
}