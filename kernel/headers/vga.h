#ifndef VGA_H
#define VGA_H

void clear_screen(unsigned char color);

void disable_cursor(void);

void port_byte_write(unsigned short port, unsigned char data);
#pragma once
extern int cursor_pos;

int cursor_set_pose(int x, int y);

void print_center(const char *text, int row);

int strlen(const char *text);

void a_print(const char *text, int row, int col, unsigned char color);

void itoa(int num, char *buf);

void strcat(char* dest, const char* strc);

char *strcpy(char *dest, const char *src);

#endif