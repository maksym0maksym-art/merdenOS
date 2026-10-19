#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/*
 * PNG-декодер MerdenOS (власний inflate, без зовнішніх бібліотек, без libgcc).
 *
 * Підтримує: усі колір-типи (gray, RGB, palette, gray+alpha, RGBA), глибину 1/2/4/8/16 біт,
 * усі п'ять PNG-фільтрів, tRNS (прозорість палітри та «прозорий колір»), IDAT, розбитий на
 * довільні шматки. Розмір до PNG_MAX_DIM x PNG_MAX_DIM.
 * НЕ підтримує: переплетення Adam7 (PNG_E_UNSUPPORTED). Контрольні суми CRC/Adler не перевіряються.
 *
 * Декодування потокове: пам'яті треба ~45 КіБ + кілька рядків, а не весь розпакований кадр,
 * тож картинка будь-якого розміру малюється одразу на екран (див. png_draw_* нижче).
 * Піксель — 0xAARRGGBB (A = 255 непрозорий).
 */

#define PNG_MAX_DIM   8192

#define PNG_OK             0
#define PNG_E_FORMAT      -1     /* не PNG / пошкоджений заголовок */
#define PNG_E_UNSUPPORTED -2     /* переплетення Adam7, дивна комбінація колір/глибина */
#define PNG_E_NOMEM       -3
#define PNG_E_CORRUPT     -4     /* пошкоджений або обрізаний потік даних */
#define PNG_E_ABORT       -5     /* колбек перервав декодування */
#define PNG_E_TOOBIG      -6     /* більше PNG_MAX_DIM */

typedef struct {
    uint32_t width, height;
    uint8_t  bit_depth;          /* 1, 2, 4, 8, 16 */
    uint8_t  color_type;         /* 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA */
    uint8_t  interlaced;
} png_info_t;

const char *png_strerror(int err);

/* Читає лише IHDR. PNG_OK або PNG_E_*. */
int png_info(const uint8_t *data, uint32_t size, png_info_t *out);

/* Зручний варіант: ширина/висота (0/0 при помилці), як bmp_get_size. */
void png_get_size(const uint8_t *data, uint32_t size, uint32_t *w, uint32_t *h);

/* Колбек рядка: y — номер рядка (0..h-1, строго по порядку), row — w пікселів 0xAARRGGBB
 * (буфер переписується наступним рядком — копіюй, якщо треба). Ненульове значення — перервати. */
typedef int (*png_row_fn)(uint32_t y, const uint32_t *row, uint32_t w, void *user);
int png_decode_rows(const uint8_t *data, uint32_t size, png_row_fn cb, void *user);

/* Декодує в готовий буфер pixels (cap_pixels штук, рядок за рядком, без відступів).
 * Якщо w*h > cap_pixels — PNG_E_NOMEM, нічого не пишеться. */
int png_load_into(const uint8_t *data, uint32_t size,
                  uint32_t *pixels, uint32_t cap_pixels, uint32_t *w, uint32_t *h);

/* Те саме, але пам'ять бере з kmalloc. NULL при помилці (код — у *err, якщо err != NULL).
 * Звільняти png_free(). */
uint32_t *png_load(const uint8_t *data, uint32_t size, uint32_t *w, uint32_t *h, int *err);
void      png_free(uint32_t *pixels);

/* ---- малювання на framebuffer (pngdraw.c); прозорість змішується з тим, що вже на екрані ---- */

/* 1:1 у точку (x, y). */
int png_draw(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y);
/* Розтягує до dst_w x dst_h (найближчий сусід). */
int png_draw_scaled(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y,
                    uint32_t dst_w, uint32_t dst_h);
/* Вписує в рамку max_w x max_h із збереженням пропорцій (меншу картинку не збільшує) і центрує в ній. */
int png_draw_fit(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y,
                 uint32_t max_w, uint32_t max_h);
/* На весь екран (розтягнуто), як bmp_draw_fullscreen. */
int png_draw_fullscreen(const uint8_t *data, uint32_t size);

#endif
