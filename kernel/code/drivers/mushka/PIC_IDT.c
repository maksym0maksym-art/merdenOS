#include "../../../headers/pic_idt.h"
#include "../../../headers/ports.h"
#include <string.h>
 
#define PIC1        0x20
#define PIC2        0xA0
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1+1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2+1)
 
#define ICW1_ICW4      0x01
#define ICW1_INIT      0x10
#define ICW4_8086      0x01
 
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));
 
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
 
static struct idt_entry idt[256];
static struct idt_ptr   idtp;
 
extern void idt_flush(uint32_t);
 
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = handler & 0xFFFF;
    idt[num].base_high = (handler >> 16) & 0xFFFF;
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}
 
void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt));
    
    idt_flush((uint32_t)&idtp);
}

void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
 
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 4);    io_wait();
    outb(PIC2_DATA, 2);    io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();
 
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}
 
void irq_set_mask(uint8_t irq_line) {
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq_line < 8 ? irq_line : irq_line - 8;
    outb(port, inb(port) | (1 << bit));
}
 
void irq_clear_mask(uint8_t irq_line) {
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq_line < 8 ? irq_line : irq_line - 8;
    outb(port, inb(port) & ~(1 << bit));
}
 
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, 0x20);
    outb(PIC1_COMMAND, 0x20);
}