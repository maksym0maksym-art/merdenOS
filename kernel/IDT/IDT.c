#include <stdint.h>
#include "../headers/system.h"
#include "../headers/IDT.h"
#include "../headers/kernel-panic.h"

#define NULL ((void*)0)


__attribute__((aligned(0x10)));
static idt_entry idt[256];

struct idt_ptr idt_ptr;

__attribute__((noreturn))
void exception_handler(void);


void idt_set_gate(
	uint8_t num,
	uint32_t base, 
	uint16_t sel,
	uint8_t flags
)
{
	idt[num].offset_low = base & 0xFFFF;
	idt[num].selector = sel;
	idt[num].zero = 0;
	idt[num].flags = flags;
	idt[num].offset_high = (base >> 16) & 0xFFFF;
}

extern void idt_load(struct idt_ptr *ptr);
//*FIXME: extern void isr0();

extern uint32_t isr_stub_table[]; 

void idt_init() 
{
	idt_ptr.limit = sizeof(idt) - 1;
	idt_ptr.base  = (uint32_t)& idt;
	
	//*цикл що додає сегменти в масив
	for(int i = 0; i < 256; i++)
{ 
	idt_set_gate(i, 0, 0, 0);
} 
/*
	idt_set_gate(
		0,
		(uint32_t)isr0,
		0x08,
		0x8E
	);
*/
	for (int i = 0; i < 32; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E); 
    }


	idt_load(&idt_ptr);
}

void exception_handler()
{
	kernel_panic(FOS_ERROR_UNKNOWN);
	asm volatile("hlt");
	kernel_halt();
	while(1);
}