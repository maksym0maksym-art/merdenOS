#include "../../../headers/usb_mem.h"

#define USB_POOL_SIZE (64 * 1024)

__attribute__((aligned(4096)))
static uint8_t usb_pool[USB_POOL_SIZE];

static uint32_t usb_pool_offset = 0;

void usb_mem_init(void) {
    usb_pool_offset = 0;
}

void* usb_alloc(uint32_t size, uint32_t align) {
    uint32_t base = (uint32_t)usb_pool + usb_pool_offset;
    uint32_t aligned = (base + (align - 1)) & ~(align - 1);
    uint32_t new_offset = (aligned - (uint32_t)usb_pool) + size;

    if (new_offset > USB_POOL_SIZE) {
        return 0;
    }
    usb_pool_offset = new_offset;
    return (void*)aligned;
}

uint32_t usb_virt_to_phys(void* virt) {
    return (uint32_t)virt;
}