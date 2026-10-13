#include "../../../headers/mouse.h"
#include "../../../headers/head_mouse.h"
#include "../../../headers/touchpad.h"
#include "../../../headers/pci_dump.h"
#include "../../../headers/xhci_hid.h"
#include "../../../headers/framebuffer.h"
#include "../../../../standarts_lib/stdio.h"
#include "../../../headers/klog.h"

static mouse_state_t    st;
static mouse_backend_t  backend = MOUSE_BACKEND_NONE;
static hid_mouse_t      usb;
static int32_t          bound_w, bound_h;
static int              xmice;          /* USB-миші через xHCI (працюють поруч з основним backend) */
static int              xhci_scanned;   /* xHCI скануємо один раз: скан скидає контролер і псує змонтовані флешки */

/* ---------------------------------------------------------------- */
/*  Програмний курсор                                                */
/*  Картинка — ARGB (0xAARRGGBB, пряма прозорість), до 64x64.        */
/*  hot x/y — точка всередині картинки, якою курсор "клікає".        */
/* ---------------------------------------------------------------- */
#define CUR_MAX 64
#define DEF_W   12
#define DEF_H   19

static const char *default_bmp[DEF_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X  X..X   ",
    "XX    X..X  ",
    "X     X..X  ",
    "       X..X ",
    "       XXX  ",
};

static uint32_t default_px[DEF_W * DEF_H];
static int      default_ready;

/* поточна картинка */
static const uint32_t *img_px;
static int img_w, img_h, img_hx, img_hy;

static uint32_t cur_under[CUR_MAX * CUR_MAX];
static int32_t  cur_x, cur_y;          /* лівий верхній кут намальованого спрайта */
static int      cur_w, cur_h;          /* його розмір на момент малювання */
static int      cur_visible;

static int fb_ok(void) {
    return framebuffer_addr != 0 && framebuffer_bpp == 32;
}

static uint32_t *fb_px(int32_t x, int32_t y) {
    return (uint32_t *)(uintptr_t)framebuffer_addr + (framebuffer_pitch / 4) * (uint32_t)y + (uint32_t)x;
}

static int in_fb(int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && (uint32_t)x < framebuffer_width && (uint32_t)y < framebuffer_height;
}

static void use_default_image(void) {
    if (!default_ready) {
        for (int j = 0; j < DEF_H; j++)
            for (int i = 0; i < DEF_W; i++) {
                char c = default_bmp[j][i];
                default_px[j * DEF_W + i] = (c == 'X') ? 0xFF000000u : (c == '.') ? 0xFFFFFFFFu : 0x00000000u;
            }
        default_ready = 1;
    }
    img_px = default_px; img_w = DEF_W; img_h = DEF_H; img_hx = 0; img_hy = 0;
}

static void cursor_restore(void) {
    for (int j = 0; j < cur_h; j++)
        for (int i = 0; i < cur_w; i++)
            if (in_fb(cur_x + i, cur_y + j))
                *fb_px(cur_x + i, cur_y + j) = cur_under[j * CUR_MAX + i];
}

static uint32_t blend(uint32_t src, uint32_t dst) {
    uint32_t a = src >> 24;
    if (a == 255) return src & 0xFFFFFF;
    if (a == 0)   return dst;
    uint32_t ia = 255 - a;
    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia + 127) / 255;
    uint32_t g = (((src >> 8)  & 255) * a + ((dst >> 8)  & 255) * ia + 127) / 255;
    uint32_t b = (( src        & 255) * a + ( dst        & 255) * ia + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

static void cursor_save_and_draw(void) {
    cur_x = st.x - img_hx;
    cur_y = st.y - img_hy;
    cur_w = img_w;
    cur_h = img_h;
    for (int j = 0; j < cur_h; j++) {
        for (int i = 0; i < cur_w; i++) {
            if (!in_fb(cur_x + i, cur_y + j)) continue;
            uint32_t *p = fb_px(cur_x + i, cur_y + j);
            cur_under[j * CUR_MAX + i] = *p;
            *p = blend(img_px[j * img_w + i], *p);
        }
    }
}

void mouse_cursor_set_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    int was_visible = cur_visible;
    mouse_cursor_hide();

    if (!argb || w < 1 || h < 1 || w > CUR_MAX || h > CUR_MAX) {
        use_default_image();
    } else {
        if (hot_x < 0) hot_x = 0;
        if (hot_y < 0) hot_y = 0;
        if (hot_x >= w) hot_x = w - 1;
        if (hot_y >= h) hot_y = h - 1;
        img_px = argb; img_w = w; img_h = h; img_hx = hot_x; img_hy = hot_y;
    }
    if (was_visible) mouse_cursor_show();
}

void mouse_cursor_show(void) {
    if (!fb_ok() || cur_visible) return;
    if (!img_px) use_default_image();
    cursor_save_and_draw();
    cur_visible = 1;
}

void mouse_cursor_hide(void) {
    if (!cur_visible) return;
    cursor_restore();
    cur_visible = 0;
}

int mouse_cursor_visible(void) {
    return cur_visible;
}

int mouse_cursor_bounds(int *x, int *y, int *w, int *h) {
    if (!cur_visible) return 0;
    *x = cur_x; *y = cur_y; *w = cur_w; *h = cur_h;
    return 1;
}

static void cursor_follow(void) {
    if (!cur_visible) return;
    if (cur_x == st.x - img_hx && cur_y == st.y - img_hy) return;
    cursor_restore();
    cursor_save_and_draw();
}

/* ---------------------------------------------------------------- */
/*  Стан                                                             */
/* ---------------------------------------------------------------- */
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void mouse_set_bounds(int32_t w, int32_t h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    bound_w = w;
    bound_h = h;
    st.x = clampi(st.x, 0, w - 1);
    st.y = clampi(st.y, 0, h - 1);
    if (backend == MOUSE_BACKEND_I2C) tp_set_screen(w, h);
}

void mouse_set_pos(int32_t x, int32_t y) {
    st.x = clampi(x, 0, bound_w - 1);
    st.y = clampi(y, 0, bound_h - 1);
    cursor_follow();
}

static void apply(int dx, int dy, int wheel, uint8_t buttons) {
    st.dx += dx;
    st.dy += dy;
    st.wheel += wheel;
    st.x = clampi(st.x + dx, 0, bound_w - 1);
    st.y = clampi(st.y + dy, 0, bound_h - 1);

    buttons &= 0x07;
    st.pressed  |= (uint8_t)(buttons & ~st.buttons);
    st.released |= (uint8_t)(st.buttons & ~buttons);
    st.buttons = buttons;
}

int mouse_init(void) {
    if (backend != MOUSE_BACKEND_NONE) return 1;

    use_default_image();

    st.x = st.y = st.dx = st.dy = st.wheel = 0;
    st.buttons = st.pressed = st.released = 0;
    mouse_set_bounds((int32_t)framebuffer_width, (int32_t)framebuffer_height);
    mouse_set_pos(bound_w / 2, bound_h / 2);

    if (!xhci_scanned) { xmice = xhci_mouse_init(); xhci_scanned = 1; }   /* сучасні ноутбуки: USB-миша на xHCI (і флешки) */

    if (hid_mouse_init(&usb)) {
        klog_ok("input: USB mouse (UHCI) ready");
        backend = MOUSE_BACKEND_USB;
        return 1;
    }
    if (ps2_mouse_init()) {
        klog_ok("input: PS/2 mouse ready");
        backend = MOUSE_BACKEND_PS2;
        return 1;
    }
    if (tp_init()) {
        tp_set_screen(bound_w, bound_h);
        klog_ok("input: I2C-HID touchpad ready");
        backend = MOUSE_BACKEND_I2C;
        return 1;
    }
    if (xmice) {
        klog_ok("input: USB mouse (xHCI) ready");
        backend = MOUSE_BACKEND_XHCI;
        return 1;
    }
    klog_warn("input: no mouse found (USB UHCI / PS/2 / I2C touchpad)");
    pci_dump_all();
    return 0;
}

int mouse_poll(void) {
    st.dx = st.dy = st.wheel = 0;
    st.pressed = st.released = 0;

    int32_t ox = st.x, oy = st.y;
    int got = 0;

    if (backend == MOUSE_BACKEND_USB) {
        mouse_report_t r;
        for (int i = 0; i < 8 && hid_mouse_poll(&usb, &r); i++) {
            apply(r.dx, r.dy, r.wheel, r.buttons);
            got = 1;
        }
    } else if (backend == MOUSE_BACKEND_I2C) {
        int dx, dy, wheel;
        uint8_t b;
        for (int i = 0; i < 16 && tp_poll(&dx, &dy, &wheel, &b); i++) {
            apply(dx, dy, wheel, b);
            got = 1;
        }
    } else if (backend == MOUSE_BACKEND_PS2) {
        int dx, dy, wheel;
        uint8_t b;
        ps2_mouse_pump();
        while (ps2_mouse_read(&dx, &dy, &wheel, &b)) {
            apply(dx, dy, wheel, b);
            got = 1;
        }
    }

    if (xmice) {                                    /* xHCI-миша: опитуємо завжди, незалежно від backend */
        int dx, dy, wheel;
        uint8_t b;
        for (int i = 0; i < 8 && xhci_mouse_read(&dx, &dy, &wheel, &b); i++) {
            apply(dx, dy, wheel, b);
            got = 1;
        }
    }

    if (got && (st.x != ox || st.y != oy)) cursor_follow();
    return got;
}

const mouse_state_t *mouse_get(void) {
    return &st;
}

mouse_backend_t mouse_backend(void) {
    return backend;
}
