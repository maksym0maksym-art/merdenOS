#include <stdint.h>
#include "gui.h"
#include "../../kernel/headers/png.h"
#include "../../kernel/headers/usbfs.h"
#include "../../kernel/headers/memory.h"

/* Переглядач PNG: відкривається з провідника (файли *.png на флешці).
 * Клавіші: F — вписати/показати як є, Esc або Q — закрити. Прозорі місця — на «шаховому» фоні. */

#define IV_COUNT        4
#define IV_MAX_FILE     (8u * 1024u * 1024u)     /* найбільший PNG-файл */
#define IV_MAX_PIXELS   (4u * 1024u * 1024u)     /* ~4 мегапікселі = 16 МіБ пам'яті */

#define C_BG        0x15162E
#define C_ERR       0xFF7B7B
#define C_CHK_A     0x3A3B5C
#define C_CHK_B     0x2C2D4A

typedef struct {
    int           used;
    gui_window_t *win;
    uint32_t     *px;                  /* 0xAARRGGBB, NULL якщо не завантажилось */
    uint32_t      w, h;
    int           has_alpha;
    int           upscale;             /* 1 — збільшувати малі картинки до розміру вікна */
    char          path[128];
    char          title[112];
    char          msg[112];
} iview_t;

static iview_t views[IV_COUNT];

/* ---------- рядки ---------- */

static void s_copy(char *dst, int sz, const char *src) {
    int i = 0;
    while (src[i] && i < sz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void s_cat(char *dst, int sz, const char *src) {
    int l = 0;
    while (dst[l]) l++;
    s_copy(dst + l, sz - l, src);
}

static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void s_cat_uint(char *dst, int sz, uint32_t v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char o[12]; int k = 0;
    while (n) o[k++] = t[--n];
    o[k] = 0;
    s_cat(dst, sz, o);
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == ':') b = p + 1;
    return b;
}

/* ---------- завантаження ---------- */

static void fail(iview_t *v, const char *what, const char *why) {
    v->msg[0] = 0;
    s_cat(v->msg, sizeof v->msg, what);
    if (why) { s_cat(v->msg, sizeof v->msg, ": "); s_cat(v->msg, sizeof v->msg, why); }
}

static void load_image(iview_t *v) {
    v->px = 0; v->w = v->h = 0; v->has_alpha = 0;

    uint8_t *buf = (uint8_t *)kmalloc(IV_MAX_FILE);
    if (!buf) { fail(v, "Not enough memory", 0); return; }

    uint32_t len = 0;
    int rc = usbfs_read(v->path, (char *)buf, IV_MAX_FILE, &len);
    if (rc) {
        fail(v, "Cannot read file", usbfs_strerror(rc));
        kfree(buf);
        return;
    }

    png_info_t info;
    int err = png_info(buf, len, &info);
    if (err == PNG_OK && (uint64_t)info.width * info.height > IV_MAX_PIXELS)
        err = PNG_E_TOOBIG;
    if (err != PNG_OK) {
        fail(v, "Cannot open image", png_strerror(err));
        kfree(buf);
        return;
    }

    uint32_t w = 0, h = 0;
    uint32_t *px = png_load(buf, len, &w, &h, &err);
    kfree(buf);
    if (!px) { fail(v, "Cannot decode image", png_strerror(err)); return; }

    for (uint32_t i = 0, n = w * h; i < n; i++)
        if ((px[i] >> 24) != 255u) { v->has_alpha = 1; break; }

    v->px = px; v->w = w; v->h = h;
}

/* ---------- вікно ---------- */

static void iv_paint(gui_window_t *win, int cx, int cy, int cw, int ch, void *ctx) {
    (void)win;
    iview_t *v = (iview_t *)ctx;
    gui_draw_rect(cx, cy, cw, ch, C_BG);

    if (!v->px) {
        gui_draw_text(cx + 12, cy + 12, v->msg, C_ERR);
        return;
    }

    int dw = (int)v->w, dh = (int)v->h;
    if (dw > cw || dh > ch || v->upscale) {
        if ((int)v->w * ch >= cw * (int)v->h) { dw = cw; dh = (int)v->h * cw / (int)v->w; }
        else                                  { dh = ch; dw = (int)v->w * ch / (int)v->h; }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    int x = cx + (cw - dw) / 2;
    int y = cy + (ch - dh) / 2;

    if (v->has_alpha) {
        for (int ty = 0; ty < dh; ty += 8)
            for (int tx = 0; tx < dw; tx += 8) {
                int tw = dw - tx < 8 ? dw - tx : 8;
                int th = dh - ty < 8 ? dh - ty : 8;
                gui_draw_rect(x + tx, y + ty, tw, th, (((tx >> 3) + (ty >> 3)) & 1) ? C_CHK_A : C_CHK_B);
            }
    }
    gui_draw_image(x, y, dw, dh, v->px, (int)v->w, (int)v->h);
}

static void iv_resize(gui_window_t *win, int cw, int ch, void *ctx) {
    (void)cw; (void)ch; (void)ctx;
    gui_window_invalidate_client(win);
}

static void iv_key(gui_window_t *win, const gui_key_t *k, void *ctx) {
    iview_t *v = (iview_t *)ctx;
    if (k->code == GUI_KEY_ESC || k->code == 'q' || k->code == 'Q') {
        gui_window_request_close(win);
    } else if (k->code == 'f' || k->code == 'F') {
        v->upscale = !v->upscale;
        gui_window_invalidate_client(win);
    }
}

static void iv_close(gui_window_t *win, void *ctx) {
    (void)win;
    iview_t *v = (iview_t *)ctx;
    png_free(v->px);
    v->px = 0;
    v->used = 0;
}

static const gui_ops_t iv_ops = {
    .paint  = iv_paint,
    .resize = iv_resize,
    .key    = iv_key,
    .close  = iv_close,
};

gui_window_t *gui_image_open(const char *path) {
    if (!path || !path[0]) return 0;

    int slot = -1;
    for (int i = 0; i < IV_COUNT; i++) {
        if (views[i].used && s_eq(views[i].path, path)) {
            gui_window_focus(views[i].win);
            return views[i].win;
        }
        if (!views[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return 0;

    iview_t *v = &views[slot];
    v->upscale = 0;
    s_copy(v->path, sizeof v->path, path);
    v->msg[0] = 0;
    load_image(v);

    s_copy(v->title, sizeof v->title, "Image: ");
    s_cat(v->title, sizeof v->title, base_name(path));
    if (v->px) {
        s_cat(v->title, sizeof v->title, "  (");
        s_cat_uint(v->title, sizeof v->title, v->w);
        s_cat(v->title, sizeof v->title, "x");
        s_cat_uint(v->title, sizeof v->title, v->h);
        s_cat(v->title, sizeof v->title, ")");
    }

    gui_window_t *win = gui_window_create(120, 60, 640, 480, v->title);
    if (!win) {
        png_free(v->px);
        v->px = 0;
        return 0;
    }

    v->used = 1;
    v->win = win;
    gui_window_set_icon(win, GUI_ICON_APP);
    gui_window_set_ops(win, &iv_ops, v);
    gui_window_focus(win);
    return win;
}
