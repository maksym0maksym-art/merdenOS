#include "../../../standarts_lib/stdio_s.h"
#include "io.h"

#define COM1 0x3F8

static int serial_ready(void)
{
    return inb(COM1 + 5) & 0x20;
}

static void serial_write(char c)
{
    while (!serial_ready());

    if (c == '\n')
        outb(COM1, '\r');

    outb(COM1, c);
}

void clear_screen(unsigned char color)
{
    (void)color;

    // ANSI: очистити екран і поставити курсор у (1,1)
    t_puts("\033[2J\033[H");
}

int cursor_pos = -1;

void port_byte_write(unsigned short port, unsigned char data)
{
    (void)port;
    (void)data;
}

int cursor_set_pose(int x, int y)
{
    (void)x;
    (void)y;
    return 0;
}

int strlen(const char *text)
{
    int len = 0;
    while (text[len] != '\0')
        len++;
    return len;
}

void print_center(const char *text, int row)
{
    (void)row;

    while (*text)
        serial_write(*text++);
}

void a_print(const char *text, int row, int col, unsigned char color)
{
    (void)row;
    (void)col;
    (void)color;

    while (*text)
        serial_write(*text++);
}

void strcat(char *dest, const char *src)
{
    int i = 0;
    int j = 0;

    while (dest[i] != '\0')
        i++;

    while (src[j] != '\0')
        dest[i++] = src[j++];

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
        char t = buf[start];
        buf[start] = buf[end];
        buf[end] = t;
        start++;
        end--;
    }
}

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