#ifndef IDT_H
#define IDT_H

void exception_handler(void);

typedef struct 
{
	uint16_t	isr_low;
	uint16_t  	offset_low;
	uint16_t    kernel_cs;
	uint8_t     reserved;
	uint8_t 	flags;
	uint8_t		zero;
	uint8_t     attributes;
	uint16_t  	selector;
	uint16_t 	isr_high;
	uint16_t offset_high;
} __attribute__((packed)) idt_entry;


void idt_set_gate(
	uint8_t num,
	uint32_t base, 
	uint16_t sel,
	uint8_t flags
);

void idt_init();



struct idt_ptr {
	uint16_t limit;
	uint32_t base;
}__attribute__((packed)) idt_ptr;

extern struct idt_ptr idt_descriptor;


extern void idt_load(struct idt_ptr *ptr);

void exception_handler();

#endif