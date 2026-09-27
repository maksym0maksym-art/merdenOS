#ifndef VGAS_H
#define VGAS_H

static int serial_ready(void);

static void serial_write(char c);

void clear_screen(unsigned char color);

void port_byte_write(unsigned short port, unsigned char data);

int cursor_set_pose(int x, int y);

int strlen(const char *text);

void print_center(const char *text, int row);

void a_print(const char *text, int row, int col, unsigned char color);

void strcat(char *dest, const char *src);

void itoa(int num, char *buf);

char *strcpy(char *dest, const char *src);

#endif 