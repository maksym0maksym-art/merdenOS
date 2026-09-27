#ifndef STDIOS_H
#define STDIOS_H

#include <stdint.h>

void t_putchar(char c);

void t_puts(const char *str);

void print_one_part(char *text, short row, short col);

int t_print_int(int num);

void t_printf(const char *format, ...);

void green_print(const char *str, uint32_t row, uint32_t col);

int strcmp(const char *a, const char *b);

void clear_intit(uint32_t x, uint32_t y);

void scrool_screen();

#endif