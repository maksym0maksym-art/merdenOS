#pragma once
#ifndef IO_H
#define IO_H

#include <stdint.h>

uint8_t inb(uint16_t port);
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t ret;
	asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}

void ide_wait_ready();

#endif
