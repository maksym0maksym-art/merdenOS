#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H
#include <stdint.h>

extern uint64_t framebuffer_addr;

extern uint32_t framebuffer_width;

extern uint32_t framebuffer_height;

extern uint32_t framebuffer_pitch;

extern uint8_t framebuffer_bpp;

void fill_screen(uint32_t color);

void put_pixel(uint32_t x, uint32_t y, uint32_t color);

void draw_char(char c, uint32_t x, uint32_t y);

void color_draw_char(char c, uint32_t x, uint32_t y, uint32_t color);

void color_draw_string(const char *str, uint32_t x, uint32_t y, uint32_t color);

void  fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void framebuffer_init(uint32_t mbi_addr);

void color_draw_string_bg(const char *str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

void color_draw_char_bg(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

#endif