//*імпорти 
#include "../drivers/io.h"
#include "../../headers/system.h"
#include "../../headers/PIC.h"

//*адреси для IO
#define PIC1 		0x20 //* базова адреса IO для pic
#define PIC2 		0xA0 //* базова адреса IO  для підлеглого pic
#define PIC1_COMMAND 	PIC1
#define PIC1_DATA 	(PIC1+1)
#define PIC2_COMMAND  PIC2
#define PIC2_DATA     (PIC2+1)

//* кінець переривання командного коду
#define PIC_EOI 0x20 

//*надсилання сигналу до EOI
void PIC_sendEOI(uint8_t irq)
{
	if(irq >= 8)
	{
		//*обробка першого PIC
		outb(PIC2_COMMAND, PIC_EOI);
	}

	//* обробка першого PIC
	outb(PIC2_COMMAND, PIC_EOI); 
}

//*повторний ініт контролерів 
#define ICW1_ICW4 0x01 //*вказує на присутність ICW4
#define ICW1_SINGLE 0x02 
#define ICW1_INTERVAL 0x08
#define ICW1_LEVEL 0x08
#define ICW_INIT 0x10

//*і ще один
#define ICW4_8086 0x01
#define ICW4_AUTO 0x02
#define ICW4_BUF_MASTER 0x0c
#define ICW4_SFNM 0x10

#define CASCADE_IRQ 2 

void PIC_remap(int offset1, int offset2)
{
//*початок ініціалізації
	outb(PIC1_COMMAND, ICW_INIT | ICW1_ICW4);
	delay;
	outb(PIC2_COMMAND, ICW_INIT | ICW1_ICW4);
	delay;
	outb(PIC1_DATA, offset1);
	delay;
	outb(PIC2_DATA, offset2);
	delay;
	outb(PIC1_DATA, 1 << CASCADE_IRQ);
	delay;
	outb(PIC2_DATA, 2);
	delay;

	outb(PIC1_DATA, ICW4_8086);
	delay;
	outb(PIC2_DATA, ICW4_8086);
	delay;

	outb(PIC1_DATA, 0);
	outb(PIC2_DATA, 0);
}

void pic_disabled(void)
{
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}

void IRQ_set_mask(
	uint8_t IRQ_line
)
{
	uint16_t port;
	uint8_t value;

	//*перевірочки
	if (IRQ_line < 8)
	{
		port = PIC1_DATA;
	}
	else
	{
		port = PIC2_DATA;
		IRQ_line -= 8;
	}
	
	value = inb(port) | (1 << IRQ_line);
	outb(port, value);
}

void IRQ_mask_clear(
	uint8_t IRQLine
)
{
	uint16_t port;
	uint8_t value;

	//*перевірка меж бітів 
	if (IRQLine < 8)
	{
		port = PIC1_DATA;
	}
	else
	{
		port = PIC2_DATA;
		IRQLine -=8;
	}

	value = inb(port) & ~(1 << IRQLine);
	outb(port, value);
}

#define PIC_READ_IRR 0x0a
#define PIC_READ_ISR 0x0b

//*можливість читання IRR, ISR і тд
static uint16_t __pic_get_irq_reg(int ocw3)
{
	outb(PIC1_COMMAND, ocw3);
	outb(PIC2_COMMAND, ocw3);

	return(inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

//*Читання IRR
uint16_t pic_get_irr(void)
{
	return __pic_get_irq_reg(PIC_READ_IRR);
}

//*Читання ISR
uint16_t pic_get_isr(void)
{
	return __pic_get_irq_reg(PIC_READ_ISR);
}