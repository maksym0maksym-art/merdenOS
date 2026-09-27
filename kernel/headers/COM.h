#ifndef COM_H
#define COM_H

static inline void outb(unsigned short port, unsigned char value);

static inline unsigned char inb(unsigned short port);

void sheral_init();

int serial_ready();

void serial_write(char c);

void serial_print(char *str);

#endif