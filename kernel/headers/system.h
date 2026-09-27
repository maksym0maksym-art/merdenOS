#ifndef SYSTEM_H
#define SYSTEM_H

void kernel_halt();

static inline void outw(unsigned short port, unsigned short value);

void poweroff();

void delay();

#endif