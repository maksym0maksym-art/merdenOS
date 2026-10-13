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

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_uhci_n(int index, pci_device_t *out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* якщо функції 0 немає, то пристрою немає взагалі (це в 8 разів менше читань) */
            if (pci_config_read16((uint8_t)bus, slot, 0, 0x00) == 0xFFFF) continue;

            uint8_t header_type = (uint8_t)((pci_config_read32((uint8_t)bus, slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t nfunc = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                uint16_t vendor = pci_config_read16((uint8_t)bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;

                uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                    if (found++ != index) continue;

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

int pci_find_uhci(pci_device_t *out) {
    return pci_find_uhci_n(0, out);
}


/* Універсальний пошук PCI-пристрою за класом (використовується xHCI-драйвером). */
int pci_find_class_n(uint8_t want_class, uint8_t want_sub, uint8_t want_pif,
                     int index, pci_device_t *out) {
    int found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16((uint8_t)bus, slot, 0, 0x00) == 0xFFFF) continue;

            uint8_t header_type = (uint8_t)((pci_config_read32((uint8_t)bus, slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t nfunc = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                uint16_t vendor = pci_config_read16((uint8_t)bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;

                uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8) & 0xFF;

                if (class_code != want_class || subclass != want_sub || prog_if != want_pif)
                    continue;
                if (found++ != index) continue;

                out->bus = (uint8_t)bus;
                out->slot = slot;
                out->func = func;
                out->vendor_id = vendor;
                out->device_id = pci_config_read16((uint8_t)bus, slot, func, 0x02);
                out->class_code = class_code;
                out->subclass = subclass;
                out->prog_if = prog_if;
                out->bar0 = pci_config_read32((uint8_t)bus, slot, func, 0x10);
                out->bar1 = pci_config_read32((uint8_t)bus, slot, func, 0x14);
                out->bar4 = pci_config_read32((uint8_t)bus, slot, func, 0x20) & ~0x3u;
                out->irq_line = (uint8_t)(pci_config_read32((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                return 1;
            }
        }
    }
    return 0;
}