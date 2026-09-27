#include "stdio.h"
#include "../kernel/headers/system.h"
#include <stdarg.h>
#include "../system/top_panel/top_panel.h"
#include "../kernel/headers/framebuffer.h"

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 18 

void t_putchar(char c)
{
	if (c == '\n') {
		cursor_x = 0;
		cursor_y += CHAR_HEIGHT;
		return;
	}
	
	draw_char(c, cursor_x, cursor_y);

	cursor_x += CHAR_WIDTH;

	if (cursor_x + CHAR_WIDTH >= framebuffer_width)
	{
		cursor_x = 0;
		cursor_x += CHAR_HEIGHT;
	}
} 

void t_puts(const char *str) {

	if (!str) return;

	while (*str) {
		t_putchar(*str);
		str++;
	}
	t_putchar('\n');
} 

void print_one_part(const char *text, int row, int col) {
	
	uint32_t x = col * CHAR_WIDTH;
	uint32_t y = row * CHAR_HEIGHT;

	int i = 0;
	int pos = (row * 80 + col) * 2;

	while(text[i] != '\0') {
		draw_char(text[i], x, y);

		x += CHAR_WIDTH;
		i++;
	}
}


void t_print_int(int num) {
	char buffer[32];
	int i = 0;
	if (num < 0) {
		t_putchar('-');
		num = -num;
	}
	do {
		buffer[i++] = (num % 10) + '0';
		num /= 10;
	} while (num > 0);

	while (i > 0) {
		t_putchar(buffer[--i]);
	}
}

void t_printf(const char *format, ...) {
	va_list args;
	va_start(args, format);
	while (*format) {
		if (*format == '%') {
			format++;
			if (*format == 'c') {
				t_putchar((char)va_arg(args, int));
				format++;
				continue;
			} else if (*format == 's') {
				t_puts(va_arg(args, char*));
				format++;
				continue;
			} else if (*format == 'd') {
				t_print_int(va_arg(args, int));
				format++;
				continue;
			} else if (*format == '%') {
				t_putchar('%');
				format++;
				continue;
			}
		} else {
			t_putchar(*format);
		}
		format++;
	}
	va_end(args);
}

uint32_t green_cursor_x = 0;
uint32_t green_cursor_y = 0;

void green_print(const char *str, uint32_t row, uint32_t col)
{
    uint32_t x = col * CHAR_WIDTH;
    uint32_t y = row * CHAR_HEIGHT;

    while (*str)
    {
        if (*str == '\n')
        {
            x = col * CHAR_WIDTH;
            y += CHAR_HEIGHT;
        }
        else
        {
            color_draw_char(*str, x, y, 0x00FF00);
            x += CHAR_WIDTH;
        }

        str++;
    }
}

int strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b)
    {
        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

int bg_color =  0x000000;

void clear_char(uint32_t x, uint32_t y)
{
    for (uint32_t py = 0; py < CHAR_HEIGHT; py++)
    {
        for (uint32_t px = 0; px < CHAR_WIDTH; px++)
        {
            put_pixel(x + px, y + py, bg_color);
        }
    }
}

#define PANEL_HEIGHT 18
#define CHAR_HEIGHT 18

/*void scrol_screen(void)
{
	uint32_t *fg = (uint32_t *)(uintptr_t)framebuffer_addr;

	for (uint32_t y = CHAR_HEIGHT; y < framebuffer_height; y++)
	{
		for (uint32_t x = 0; x < framebuffer_width; x++)
		{
    	uint32_t *fb = (uint32_t *)(uintptr_t)framebuffer_addr;
			fb[(y - CHAR_HEIGHT) * (framebuffer_pitch / 4) + x] =
				fb[y *(framebuffer_pitch / 4) + x];
		}
	}

	for (uint32_t y = framebuffer_height - CHAR_HEIGHT; y < framebuffer_height; y++)
	{
		for(uint32_t x = 0; x < framebuffer_width; x++)
		{
			uint32_t *fb = (uint32_t *)(uintptr_t)framebuffer_addr;
			fb[y * (framebuffer_pitch / 4) + x] = 0x00000;
		}
	}

	draw_top_panel();
}*/

void t_printf_hex(uint32_t num)
{
	char hex[32] = "0123456789ABCDEF";

	t_printf("0x");

	for (int i = 28; i >= 0; i -= 4)
	{
		t_putchar(hex[(num >> i) & 0xF]);
	}
}

void print_one_part_color(const char *text, int row, int col, uint32_t color)
{
    uint32_t x = col * CHAR_WIDTH;
    uint32_t y = row * CHAR_HEIGHT;

    int i = 0;

    while (text[i] != '\0')
    {
        color_draw_char(text[i], x, y, color);
        x += CHAR_WIDTH;
        i++;
    }
}

void print_brackets_green(const char *str, uint32_t row, uint32_t col)
{
    uint32_t x = col * CHAR_WIDTH;
    uint32_t y = row * CHAR_HEIGHT;

    uint32_t color = 0xFFFFFF; // білий

    while (*str)
    {
        if (*str == '[')
            color = 0x00FF00; // зелений

        color_draw_char(*str, x, y, color);

        if (*str == ']')
            color = 0xFFFFFF; // назад на білий

        x += CHAR_WIDTH;
        str++;
    }
	delay();
}