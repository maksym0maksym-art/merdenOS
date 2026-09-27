#ifndef BMP_H
#define BMP_H

#include <stdint.h>

/*
 * bmp_draw — малює вбудований (у пам'яті) 24-bit uncompressed BMP
 * у framebuffer, починаючи з координати (x, y).
 *
 * data      — вказівник на сирі байти BMP-файлу (наприклад doge_data)
 * data_size — розмір масиву в байтах (наприклад doge_size)
 * x, y      — координата верхнього лівого кута зображення на екрані
 *
 * Повертає 0 при успіху, -1 якщо файл некоректний або має
 * непідтримуваний формат (стиснення, палітра, не 24 біт/піксель).
 */
int bmp_draw(const uint8_t *data, uint32_t data_size, uint32_t x, uint32_t y);

/* Повертає ширину/висоту BMP без малювання (0/0 при помилці). */
void bmp_get_size(const uint8_t *data, uint32_t data_size,
                   uint32_t *out_width, uint32_t *out_height);

int bmp_draw_scaled(const uint8_t *data, uint32_t data_size,
                     uint32_t x, uint32_t y,
                     uint32_t dst_w, uint32_t dst_h);


int bmp_draw_fullscreen(const uint8_t *data, uint32_t data_size);

#endif // BMP_H