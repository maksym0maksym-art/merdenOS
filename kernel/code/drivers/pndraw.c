//*малювання PNG на framebuffer: 1:1, масштаб (найближчий сусід), вписування в рамку; alpha змішується з екраном
#include <stdint.h>
#include <stddef.h>

#ifdef PNG_HOST_TEST
#include "pngdraw_host.h"                 /* заглушки framebuffer для тесту на хості */
#else
#include "../../headers/png.h"
#include "../../headers/framebuffer.h"
#endif

#define DRAW_MAX_DIM  16384u              /* щоб dx * sw гарантовано влізло в 32 біти */

typedef struct {
    uint32_t x, y;                        /* лівий верхній кут призначення */
    uint32_t sw, sh;                      /* розмір джерела */
    uint32_t dw, dh;                      /* розмір призначення */
    uint32_t dy;                          /* наступний рядок призначення, який треба намалювати */
} draw_ctx_t;

//*один піксель із прозорістю
static inline uint32_t blend_ch(uint32_t s, uint32_t b, uint32_t a)
{
    uint32_t t = s * a + b * (255u - a) + 128u;
    return (t + (t >> 8)) >> 8;           /* точне ділення на 255 */
}

static void plot(uint32_t px, uint32_t py, uint32_t argb)
{
    uint32_t a = argb >> 24;
    if (a == 0)
        return;

    uint32_t color = argb & 0x00FFFFFFu;
    if (a != 255u) {
        uint32_t bg = 0;
        if (framebuffer_bpp == 32)
            bg = *(volatile uint32_t *)(uintptr_t)(framebuffer_addr
                    + (uint64_t)py * framebuffer_pitch + (uint64_t)px * 4u) & 0x00FFFFFFu;
        uint32_t r = blend_ch((color >> 16) & 255u, (bg >> 16) & 255u, a);
        uint32_t g = blend_ch((color >> 8)  & 255u, (bg >> 8)  & 255u, a);
        uint32_t b = blend_ch(color & 255u,         bg & 255u,         a);
        color = (r << 16) | (g << 8) | b;
    }
    put_pixel(px, py, color);
}

//*колбек рядка джерела: малює всі рядки призначення, що відповідають цьому рядку
static int draw_row(uint32_t sy, const uint32_t *row, uint32_t w, void *user)
{
    (void)w;
    draw_ctx_t *c = (draw_ctx_t *)user;

    while (c->dy < c->dh && (c->dy * c->sh) / c->dh == sy) {
        uint32_t py = c->y + c->dy;
        if (py >= framebuffer_height)
            return 1;                      /* нижче екрана — далі малювати нічого */

        for (uint32_t dx = 0; dx < c->dw; dx++) {
            uint32_t px = c->x + dx;
            if (px >= framebuffer_width)
                break;
            plot(px, py, row[(dx * c->sw) / c->dw]);
        }
        c->dy++;
    }
    return c->dy >= c->dh;                 /* усе намальовано — решту рядків можна не розпаковувати */
}

static int draw_core(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y,
                     uint32_t dw, uint32_t dh)
{
    png_info_t inf;
    int rc = png_info(data, size, &inf);
    if (rc != PNG_OK)
        return rc;
    if (dw == 0 || dh == 0 || dw > DRAW_MAX_DIM || dh > DRAW_MAX_DIM)
        return PNG_E_TOOBIG;

    draw_ctx_t c = { x, y, inf.width, inf.height, dw, dh, 0 };
    rc = png_decode_rows(data, size, draw_row, &c);
    if (rc == PNG_E_ABORT)                     /* ми самі зупинили: усе вже намальовано або нижче екрана */
        rc = PNG_OK;
    return rc;
}

int png_draw(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y)
{
    uint32_t w, h;
    png_get_size(data, size, &w, &h);
    if (w == 0)
        return PNG_E_FORMAT;
    return draw_core(data, size, x, y, w, h);
}

int png_draw_scaled(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y,
                    uint32_t dst_w, uint32_t dst_h)
{
    return draw_core(data, size, x, y, dst_w, dst_h);
}

int png_draw_fit(const uint8_t *data, uint32_t size, uint32_t x, uint32_t y,
                 uint32_t max_w, uint32_t max_h)
{
    uint32_t iw, ih;
    png_get_size(data, size, &iw, &ih);
    if (iw == 0)
        return PNG_E_FORMAT;
    if (max_w == 0 || max_h == 0 || max_w > DRAW_MAX_DIM || max_h > DRAW_MAX_DIM)
        return PNG_E_TOOBIG;

    uint32_t dw = iw, dh = ih;
    if (iw > max_w || ih > max_h) {
        if ((uint64_t)iw * max_h >= (uint64_t)max_w * ih) {   /* впирається в ширину */
            dw = max_w;
            dh = (ih * max_w) / iw;
        } else {                                              /* впирається у висоту */
            dh = max_h;
            dw = (iw * max_h) / ih;
        }
        if (dw == 0) dw = 1;
        if (dh == 0) dh = 1;
    }
    return draw_core(data, size, x + (max_w - dw) / 2, y + (max_h - dh) / 2, dw, dh);
}

int png_draw_fullscreen(const uint8_t *data, uint32_t size)
{
    return draw_core(data, size, 0, 0, framebuffer_width, framebuffer_height);
}
