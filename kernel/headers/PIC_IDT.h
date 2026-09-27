#ifndef PIC_IDT_H
#define PIC_IDT_H

#include <stdint.h>

void pic_remap(void);
void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t flags);
void irq_set_mask(uint8_t irq_line);
void irq_clear_mask(uint8_t irq_line);
void pic_send_eoi(uint8_t irq);

#endif