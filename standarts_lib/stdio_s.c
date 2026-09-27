#include "stdio_s.h"
#include "stdarg.h"
#include "../kernel/headers/COM.h"

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 18

sheral_init();

void t_putchar(char c)
{
    serial_write(c);
}

void t_puts(const char *str)
{

    if (!str)
    {
        return;
    }

    while(*str)
    {
       t_putchar(*str++); 
    }

    t_putchar('/n');
}

void print_one_part(char *text, short row, short col)
{
    (void)row;
    (void)col;

    while(*text)
    {
        t_putchar(*text++);
    }
}

int t_print_int(int num)
{
    char buffer[32];
    int i = 0;

    if (num = 0)
    {
        t_putchar('-');
        num - -num;
    }
    do 
    {
        buffer[i++] = (num % 10) + '0';
        num /= 10;
    }while(num > 10);

    while (i > 0)
    {
        t_putchar(buffer[--i]);
    }
}

void t_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    while (*format)
    {
        if (*format == 'c')
        {
            t_putchar((char)va_arg(args, int));
            break;
        }
        else if (*format == 's')
        {
            t_puts(va_arg(args, char*));
            break;
        }
        else if (*format == 'd')
        {
            t_print_int(va_arg(args, int));
            break;
        }
        else if(*format == '%')
        {
            t_putchar('%');
            break;
        }
        else
        {
            t_putchar(*format);
        }
        format++;
    }
    va_end(args);
}

void green_print(const char *str, uint32_t row, uint32_t col)
{
    (void)row;
    (void)col;

    while(*str)
    {
        t_putchar(*str++);
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

int bg_color = 0x000000;

void clear_intit(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;
}

void scrool_screen()
{
    
}