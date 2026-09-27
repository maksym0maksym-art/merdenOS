#include <stdint.h>
#include "../../../standarts_lib/stdio.h"
#include "../../headers/vga.h"
#include "io.h"
#include "../../headers/framebuffer.h"

//uint32_t *fb = bootInfo->framebuffer

void clear_screen(unsigned char color) {
	fill_screen(color);
}
int cursor_pos = -1;

void port_byte_write(unsigned short port, unsigned char data) 
{
	__asm__("out %%al, %%dx" : : "a"(data), "d"(port ));
}

int cursor_set_pose(int x, int y) {
	int pos = y * 80 + x;
	cursor_pos=pos;
    port_byte_write(0x3D4, 14);
	port_byte_write(0x3D5, pos >> 8);
	port_byte_write(0X3D4, 15);
	port_byte_write(0X3D5, pos);
}

int strlen(const char *text)
{
	int len = 0;
	while(text[len] != '\0') len++;
	return len;
}

void print_center(const char *text, int row)
{
	int len = strlen(text);

	uint32_t x = (framebuffer_width - len * 8) / 2;
	uint32_t y = row * 18;

	while(*text)
	{
		draw_char(*text, x, y);
		x += 8;
		text++;
	}
}	

void a_print(const char *text, int row, int col, unsigned char color)
{
	uint32_t x = col * 8;
	uint32_t y = row * 18;

	while(*text)
	{
		color_draw_char(*text, x, y, color);

		x += 8;
		text++;
	}
}

void strcat(char* dest, const char* strc)
{
    int j = 0;
    int i = 0;
    while(dest[i] != '\0') i++;
    while(strc[j] != '\0') dest[i++] = strc[j++];
    dest[i] = '\0';
}


void itoa(int num, char *buf)
{
    if (num == 0)
    {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    int i = 0;
    int start = 0;

    if (num < 0)
    {
        buf[i++] = '-';
        num = -num;
        start = 1;
    }

    while (num > 0)
    {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    buf[i] = '\0';

    int end = i - 1;

    while (start < end)
    {
        char temp = buf[start];
        buf[start] = buf[end];
        buf[end] = temp;
        start++;
        end--;
    }
}

/*
void scrol_screen(void)
{
	uint16_t *video = (uint16_t *)0xB8000;

	for (int y = 1; y < 25; y++)
	{
		for (int x = 0;x < 80; x++)
		{
			video[(y - 1) * 80 + x] = video[y * 80 + x];
		}
	}

	for (int x = 0; x < 80; x++)
	{
		video[24 * 80 + x] = (0x0f << 8) | ' ';
	}
}*/

char *strcpy(char *dest, const char *src)
{
	int i = 0;

	while (src[i] != '\0')
	{
		dest[i] = src[i];
		i++;
	}

	dest[i] = '\0';
	return dest;
}