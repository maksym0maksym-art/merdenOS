#include "../../headers/COM.h"
#define COM 0x3F8

static inline void outb(unsigned short port, unsigned char value)
{
    asm volatile("outb %0 %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char ret;
    asm volatile("inb %0 %1" : "=a"(ret) : "Nd"(port));
    return ret;
}

void sheral_init()
{
    outb(COM + 1, 0x00); //переривання 
    outb(COM + 3, 0x80); // DLAB
    outb(COM + 0, 0x03);
    outb(COM + 1, 0x00);
    outb(COM + 3, 0x03);
    outb(COM + 2, 0xC7);
    outb(COM + 4, 0x0B);
}

int serial_ready()
{
    return inb(COM + 5) & 0x20;
}

void serial_write(char c)
{
    while(!serial_ready());
    outb(COM, c);
}

void serial_print(char *str)
{
    while(*str)
    {
        serial_write(*str++);
    }
}