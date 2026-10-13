#include "../../../headers/mouse_cmd.h"
#include "../../../headers/mouse.h"
#include "../../../headers/touchpad.h"
#include "../../../headers/framebuffer.h"
#include "../io.h"
#include "../../../../standarts_lib/stdio.h"
#include "../../../../system/shell/shell.h"
#include "../../../../system/shell/graphic/console.h"

/*
 * Команда `mouse`: повноекранний тест. Малюй лівою кнопкою, права — очистити,
 * середня — змінити колір, колесо — розмір пензля, ESC — вихід.
 * Це заодно приклад того, як користуватись mouse.h у майбутньому GUI.
 */

static int g_ready = 0;

#define CANVAS_TOP 64

static char *put_str(char *d, const char *s) {
    while (*s) *d++ = *s++;
    return d;
}

static char *put_int(char *d, int v) {
    char t[12];
    int n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) *d++ = t[--n];
    return d;
}

static void draw_status(const mouse_state_t *m, int brush) {
    char line[160];
    char *p = line;

    p = put_str(p, "x=");   p = put_int(p, m->x);
    p = put_str(p, " y=");  p = put_int(p, m->y);
    p = put_str(p, "  btn=");
    *p++ = (m->buttons & MOUSE_BTN_LEFT)   ? 'L' : '-';
    *p++ = (m->buttons & MOUSE_BTN_MIDDLE) ? 'M' : '-';
    *p++ = (m->buttons & MOUSE_BTN_RIGHT)  ? 'R' : '-';
    p = put_str(p, "  brush="); p = put_int(p, brush);
    p = put_str(p, "  wheel="); p = put_int(p, m->wheel);
    if (mouse_backend() == MOUSE_BACKEND_I2C) {
        int ver, ok, sent; uint32_t nt, nm;
        tp_mode_info(&ver, &ok, &sent, &nt, &nm);
        p = put_str(p, "  fingers="); p = put_int(p, tp_fingers());
        p = put_str(p, " ptp="); p = put_str(p, ver ? "OK" : "NO");
        p = put_str(p, " set="); p = put_int(p, ok); *p++ = '/'; p = put_int(p, sent);
        p = put_str(p, " rpt(t/m)="); p = put_int(p, (int)nt); *p++ = '/'; p = put_int(p, (int)nm);
    }
    while (p - line < 110) *p++ = ' ';      /* затерти хвіст попереднього рядка */
    *p = 0;

    color_draw_string_bg(line, 16, 40, 0x00FF66, 0x000000);
}

static void draw_line(int x0, int y0, int x1, int y1, int brush, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        int y = y0 - brush / 2;
        if (y >= CANVAS_TOP)
            fill_rect((uint32_t)(x0 - brush / 2 < 0 ? 0 : x0 - brush / 2), (uint32_t)y,
                      (uint32_t)brush, (uint32_t)brush, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void cmd_mouse_test(void)
{
    if (!g_ready)
    {
        shell_print("Init mouse (USB UHCI first, then PS/2)...\n");
        if (!mouse_init())
        {
            shell_print("Mouse not found (no USB UHCI mouse and no PS/2 mouse)\n");
            return;
        }
        g_ready = 1;
    }

    console_enter_alt();

    color_draw_string_bg(mouse_backend() == MOUSE_BACKEND_USB ? "MOUSE TEST  [USB]"
                         : mouse_backend() == MOUSE_BACKEND_I2C ? "MOUSE TEST  [I2C TOUCHPAD]" : "MOUSE TEST  [PS/2]",
                         16, 22, 0xFFFFFF, 0x000000);
    color_draw_string_bg("LMB paint | RMB clear | MMB color | wheel brush | ESC exit",
                         200, 22, 0x888888, 0x000000);

    static const uint32_t palette[] = { 0xFFFFFF, 0xFF4040, 0x40FF40, 0x4080FF, 0xFFFF40, 0xFF40FF };
    int color_idx = 0;
    int brush = 3;

    mouse_set_pos((int32_t)(framebuffer_width / 2), (int32_t)(framebuffer_height / 2));
    mouse_poll();                                   /* скинути накопичене */
    draw_status(mouse_get(), brush);
    mouse_cursor_show();

    int32_t last_x = mouse_get()->x, last_y = mouse_get()->y;

    for (;;)
    {
        /* ESC. Байти мишки (біт 0x20) тут не чіпаємо — їх забирає mouse_poll() */
        uint8_t s = inb(0x64);
        if ((s & 0x21) == 0x01)
        {
            if (inb(0x60) == 0x01)
                break;
        }

        if (!mouse_poll())
            continue;

        const mouse_state_t *m = mouse_get();

        mouse_cursor_hide();                        /* малюємо на чистому екрані, без "слідів" курсора */

        if (m->wheel)
        {
            brush += m->wheel;
            if (brush < 1) brush = 1;
            if (brush > 15) brush = 15;
        }
        if (m->pressed & MOUSE_BTN_MIDDLE)
            color_idx = (color_idx + 1) % (int)(sizeof(palette) / sizeof(palette[0]));
        if (m->pressed & MOUSE_BTN_RIGHT)
            fill_rect(0, CANVAS_TOP, framebuffer_width, framebuffer_height - CANVAS_TOP, 0x000000);

        if (m->buttons & MOUSE_BTN_LEFT)
        {
            int px = (m->pressed & MOUSE_BTN_LEFT) ? m->x : last_x;
            int py = (m->pressed & MOUSE_BTN_LEFT) ? m->y : last_y;
            draw_line(px, py, m->x, m->y, brush, palette[color_idx]);
        }
        last_x = m->x;
        last_y = m->y;

        draw_status(m, brush);
        mouse_cursor_show();
    }

    mouse_cursor_hide();
    console_exit_alt();
    shell_print("Stopped.\n");
}
