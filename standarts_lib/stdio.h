#ifndef STDIO_H
#define STDIO_H
#include <stdint.h>

static uint16_t * const VGA_MEMORY = (uint16_t *)0xB8000;

static int cursor_y = 0;
static int cursor_x = 0;

void t_putchar(char c);

void t_puts(const char *str);

void print_one_part(const char *text, int row, int col);

void t_print_int(int num);

void t_printf(const char *format, ...);

int strcmp(const char *a, const char *b);

void green_print(const char *str, uint32_t x, uint32_t y);

void clear_char(uint32_t x, uint32_t y);

void t_printf_hex(uint32_t num);

void scrol_screen(void);

void print_one_part_color(const char *text, int row, int col, uint32_t color);

void print_brackets_green(const char *str, uint32_t row, uint32_t col);

static inline void ms_delay(void)
{
    asm volatile (
        "mov $0xFFFFFFFF, %%ecx\n"
        "1: loop 1b\n"
        :
        :
        : "ecx"
    );
}

#endif