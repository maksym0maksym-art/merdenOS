#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void PIC_sendEOI(uint8_t irq);

void PIC_remap(int offset1, int offset2);


void pic_disabled(void);

void IRQ_set_mask(
	uint8_t IRQ_line
);

void IRQ_mask_clear(
	uint8_t IRQLine
);

static uint16_t __pic_get_irq_reg(int ocw3);

uint16_t pic_get_irr(void);

uint16_t pic_get_isr(void);

#endif 