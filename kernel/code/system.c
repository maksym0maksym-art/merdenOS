#include "../headers/system.h"

//from merdenOS

void kernel_halt()
{ 
  asm volatile("cli");
   while(1) {
     asm("hlt");
    }
}

static inline void outw(unsigned short port, unsigned short value)
{
  asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void poweroff()
{
   outw(0xF4, 0x00);
}

void delay()
{
    for (volatile long i = 0; i < 20000000; i++) 
  {
      asm("nop");
    }
}