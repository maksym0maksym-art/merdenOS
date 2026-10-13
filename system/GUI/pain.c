#include <stdint.h>

#include "gui.h"
#include "paint.h"

#define CV_W           1280
#define CV_H            800

#define TB_H             40
#define PAL_W            56

#define BRUSH_MIN         1
#define BRUSH_MAX        32

#define C_UI       0xD9D9D9
#define C_BACK     0x808080
#define C_WHITE    0xFFFFFF
#define C_BLACK    0x000000
#define C_FACE     0xD4D0C8
#define C_DARK     0x808080

#define TOOL_Y           6
#define TOOL_W          30
#define TOOL_H          28

#define TOOL_X(i)       (6 + (i) * 34)

#define SIZE_LBL_X     154
#define MINUS_X        198
#define VAL_X          222
#define VAL_W           26
#define PLUS_X         248
#define SM_W            22

#define COLOR_LBL_X    282
#define SWATCH_X       334
#define SWATCH_W        40

#define CLEAR_X        386
#define CLEAR_W         56

enum
{
    T_PENCIL = 0,
    T_FILL,
    T_FRAME,
    T_ERASER,
    T_COUNT
};

static uint32_t canvas[CV_W * CV_H];

static const uint32_t palette[16] =
{
    0x000000, 0xFFFFFF,
    0x808080, 0xC0C0C0,

    0xE02020, 0x800000,
    0xFF8000, 0xFFE000,

    0x20B020, 0x006000,
    0x00C0C0, 0x2060E0,

    0x000080, 0x8020C0,
    0xFF80C0, 0x804000
};

static struct
{
    int tool;
    int size;

    uint32_t color;

    int drawing;     /* тягнемо фігуру */
    int stroking;    /* малюємо лінію олівцем / гумкою */

    int lx;
    int ly;

    int sx;
    int sy;

    int ex;
    int ey;

} P;

static gui_window_t *pwin = 0;

/* параметри попереднього перегляду фігури (екранні координати полотна) */
static int prev_ox, prev_oy, prev_w, prev_h;

static int pmin(int a, int b)
{
    return a < b ? a : b;
}

static int pmax(int a, int b)
{
    return a > b ? a : b;
}

static int pclamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;

    if (v > hi)
        return hi;

    return v;
}

static int in_box(int px, int py, int x, int y, int w, int h)
{
    return px >= x &&
           py >= y &&
           px < x + w &&
           py < y + h;
}

static int is_shape(void)
{
    return P.tool == T_FILL ||
           P.tool == T_FRAME;
}

static int brush_size(void)
{
    if (P.tool == T_ERASER)
        return P.size * 3;

    return P.size;
}

static uint32_t brush_color(void)
{
    if (P.tool == T_ERASER)
        return C_WHITE;

    return P.color;
}

static int pal_x(int i)
{
    return 5 + (i & 1) * 26;
}

static int pal_y(int i)
{
    return TB_H + 8 + (i >> 1) * 28;
}

/* ---------- полотно ---------- */

static void cv_fill(int x, int y, int w, int h, uint32_t color)
{
    int x0 = pmax(x, 0);
    int y0 = pmax(y, 0);

    int x1 = pmin(x + w, CV_W);
    int y1 = pmin(y + h, CV_H);

    if (x0 >= x1 || y0 >= y1)
        return;

    for (int y2 = y0; y2 < y1; y2++)
    {
        uint32_t *p = &canvas[y2 * CV_W + x0];

        for (int x2 = x0; x2 < x1; x2++)
            *p++ = color;
    }
}

static void cv_clear(void)
{
    cv_fill(0, 0, CV_W, CV_H, C_WHITE);
}

static void cv_line(int x0, int y0, int x1, int y1, int size, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;

    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;

    int err = dx - dy;

    for (;;)
    {
        cv_fill(x0 - size / 2, y0 - size / 2, size, size, color);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;

        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }

        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

typedef void (*fill_fn)(int x, int y, int w, int h, uint32_t color);

static void shape_draw(fill_fn fill)
{
    int x0 = pmin(P.sx, P.ex);
    int y0 = pmin(P.sy, P.ey);

    int x1 = pmax(P.sx, P.ex);
    int y1 = pmax(P.sy, P.ey);

    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;

    if (P.tool == T_FILL)
    {
        fill(x0, y0, w, h, P.color);
        return;
    }

    int t = pmin(P.size, pmin(w, h));

    fill(x0,         y0,         w, t, P.color);
    fill(x0,         y0 + h - t, w, t, P.color);
    fill(x0,         y0,         t, h, P.color);
    fill(x0 + w - t, y0,         t, h, P.color);
}

/* малює фігуру поверх вікна (не чіпаючи полотна), обрізаючи по області полотна */
static void preview_fill(int x, int y, int w, int h, uint32_t c)
{
    int x0 = pmax(x, 0);
    int y0 = pmax(y, 0);

    int x1 = pmin(x + w, prev_w);
    int y1 = pmin(y + h, prev_h);

    if (x0 >= x1 || y0 >= y1)
        return;

    gui_draw_rect(prev_ox + x0, prev_oy + y0, x1 - x0, y1 - y0, c);
}

/* ---------- перемальовування ---------- */

static void inv_canvas(gui_window_t *w, int x, int y, int ww, int hh)
{
    int cx;
    int cy;

    gui_window_client(w, &cx, &cy, 0, 0);

    gui_invalidate_rect(cx + PAL_W + x, cy + TB_H + y, ww, hh);
}

static void inv_seg(gui_window_t *w, int x0, int y0, int x1, int y1, int size)
{
    int margin = size / 2 + 2;

    int ax = pmin(x0, x1) - margin;
    int ay = pmin(y0, y1) - margin;

    int aw = pmax(x0, x1) - pmin(x0, x1) + 2 * margin + 1;
    int ah = pmax(y0, y1) - pmin(y0, y1) + 2 * margin + 1;

    inv_canvas(w, ax, ay, aw, ah);
}

static void inv_shape(gui_window_t *w)
{
    inv_seg(w, P.sx, P.sy, P.ex, P.ey, P.size);
}

static void inv_ui(gui_window_t *w)
{
    int cx;
    int cy;
    int cw;
    int ch;

    gui_window_client(w, &cx, &cy, &cw, &ch);

    gui_invalidate_rect(cx, cy, cw, TB_H);
    gui_invalidate_rect(cx, cy + TB_H, PAL_W, ch - TB_H);
}

static int vis(int x, int y, int w, int h)
{
    int cx;
    int cy;
    int cw;
    int ch;

    gui_get_clip(&cx, &cy, &cw, &ch);

    return x < cx + cw &&
           cx < x + w &&
           y < cy + ch &&
           cy < y + h;
}

/* ---------- малювання ---------- */

static void draw_canvas(int x, int y, int w, int h)
{
    int clip_x;
    int clip_y;
    int clip_w;
    int clip_h;

    gui_get_clip(&clip_x, &clip_y, &clip_w, &clip_h);

    int x0 = pmax(x, clip_x);
    int y0 = pmax(y, clip_y);

    int x1 = pmin(x + pmin(w, CV_W), clip_x + clip_w);
    int y1 = pmin(y + pmin(h, CV_H), clip_y + clip_h);

    if (x0 >= x1 || y0 >= y1)
        return;

    int canvas_x0 = x0 - x;
    int canvas_y0 = y0 - y;

    int canvas_x1 = x1 - x;
    int canvas_y1 = y1 - y;

    for (int py = canvas_y0; py < canvas_y1; py++)
    {
        int start = canvas_x0;

        while (start < canvas_x1)
        {
            uint32_t color = canvas[py * CV_W + start];

            int end = start + 1;

            while (end < canvas_x1 && canvas[py * CV_W + end] == color)
                end++;

            gui_draw_rect(x + start, y + py, end - start, 1, color);

            start = end;
        }
    }
}

static void ui_btn(int x, int y, int w, int h, int down)
{
    uint32_t tl = down ? C_DARK : C_WHITE;
    uint32_t br = down ? C_WHITE : C_DARK;

    gui_draw_rect(x, y, w, h, C_FACE);

    gui_draw_rect(x, y, w, 1, tl);
    gui_draw_rect(x, y, 1, h, tl);

    gui_draw_rect(x, y + h - 1, w, 1, br);
    gui_draw_rect(x + w - 1, y, 1, h, br);
}

static void put_num(int value, char *buffer)
{
    if (value >= 10)
    {
        buffer[0] = (char)('0' + value / 10);
        buffer[1] = (char)('0' + value % 10);
        buffer[2] = 0;
    }
    else
    {
        buffer[0] = (char)('0' + value);
        buffer[1] = 0;
    }
}

static void draw_tool_icon(int i, int x, int y)
{
    switch (i)
    {
        case T_PENCIL:

            for (int k = 0; k < 12; k++)
                gui_draw_rect(x + 8 + k, y + 18 - k, 3, 3, C_BLACK);

            gui_draw_rect(x + 6, y + 19, 3, 3, C_BLACK);

            break;

        case T_FILL:

            gui_draw_rect(x + 8, y + 7, 14, 14, C_BLACK);

            break;

        case T_FRAME:

            gui_draw_rect(x + 8,  y + 7,  14, 2,  C_BLACK);
            gui_draw_rect(x + 8,  y + 19, 14, 2,  C_BLACK);
            gui_draw_rect(x + 8,  y + 7,  2,  14, C_BLACK);
            gui_draw_rect(x + 20, y + 7,  2,  14, C_BLACK);

            break;

        case T_ERASER:

            gui_draw_text(x + 11, y + 5, "E", C_BLACK);

            break;
    }
}

static void paint_cb(gui_window_t *win, int cx, int cy, int cw, int ch, void *ctx)
{
    (void)win;
    (void)ctx;

    int ax = cx + PAL_W;
    int ay = cy + TB_H;

    int aw = cw - PAL_W;
    int ah = ch - TB_H;

    if (aw > 0 && ah > 0 && vis(ax, ay, aw, ah))
    {
        int bw = pmin(aw, CV_W);
        int bh = pmin(ah, CV_H);

        draw_canvas(ax, ay, bw, bh);

        if (aw > bw)
            gui_draw_rect(ax + bw, ay, aw - bw, ah, C_BACK);

        if (ah > bh)
            gui_draw_rect(ax, ay + bh, bw, ah - bh, C_BACK);

        /* попередній перегляд фігури, поки тягнемо мишу */
        if (P.drawing && is_shape())
        {
            prev_ox = ax;
            prev_oy = ay;
            prev_w  = bw;
            prev_h  = bh;

            shape_draw(preview_fill);
        }
    }

    if (ch > TB_H && vis(cx, cy + TB_H, PAL_W, ch - TB_H))
    {
        gui_draw_rect(cx, cy + TB_H, PAL_W, ch - TB_H, C_UI);
        gui_draw_rect(cx + PAL_W - 1, cy + TB_H, 1, ch - TB_H, C_DARK);

        for (int i = 0; i < 16; i++)
        {
            int x = cx + pal_x(i);
            int y = cy + pal_y(i);

            if (P.color == palette[i])
            {
                gui_draw_rect(x - 2, y - 2, 26, 26, C_BLACK);
                gui_draw_rect(x - 1, y - 1, 24, 24, C_WHITE);
            }
            else
            {
                gui_draw_rect(x - 1, y - 1, 24, 24, C_DARK);
            }

            gui_draw_rect(x, y, 22, 22, palette[i]);
        }
    }

    if (vis(cx, cy, cw, TB_H))
    {
        gui_draw_rect(cx, cy, cw, TB_H, C_UI);
        gui_draw_rect(cx, cy + TB_H - 1, cw, 1, C_DARK);

        for (int i = 0; i < T_COUNT; i++)
        {
            int x = cx + TOOL_X(i);
            int y = cy + TOOL_Y;

            int down = P.tool == i;

            ui_btn(x, y, TOOL_W, TOOL_H, down);

            draw_tool_icon(i, x + down, y + down);
        }

        gui_draw_text(cx + SIZE_LBL_X, cy + TOOL_Y + 5, "Size:", C_BLACK);

        ui_btn(cx + MINUS_X, cy + TOOL_Y, SM_W, TOOL_H, 0);
        gui_draw_text(cx + MINUS_X + 7, cy + TOOL_Y + 5, "-", C_BLACK);

        ui_btn(cx + PLUS_X, cy + TOOL_Y, SM_W, TOOL_H, 0);
        gui_draw_text(cx + PLUS_X + 7, cy + TOOL_Y + 5, "+", C_BLACK);

        char num[3];

        put_num(P.size, num);

        int n = num[1] ? 2 : 1;

        gui_draw_text(cx + VAL_X + (VAL_W - 8 * n) / 2,
                      cy + TOOL_Y + 5,
                      num,
                      C_BLACK);

        gui_draw_text(cx + COLOR_LBL_X, cy + TOOL_Y + 5, "Color:", C_BLACK);

        gui_draw_rect(cx + SWATCH_X, cy + TOOL_Y, SWATCH_W, TOOL_H, C_BLACK);

        gui_draw_rect(cx + SWATCH_X + 1,
                      cy + TOOL_Y + 1,
                      SWATCH_W - 2,
                      TOOL_H - 2,
                      P.color);

        ui_btn(cx + CLEAR_X, cy + TOOL_Y, CLEAR_W, TOOL_H, 0);
        gui_draw_text(cx + CLEAR_X + 8, cy + TOOL_Y + 5, "Clear", C_BLACK);
    }
}

/* ---------- ввід ---------- */

static void set_size(gui_window_t *w, int size)
{
    size = pclamp(size, BRUSH_MIN, BRUSH_MAX);

    if (size == P.size)
        return;

    P.size = size;

    inv_ui(w);
}

static void toolbar_click(gui_window_t *w, int x, int y)
{
    for (int i = 0; i < T_COUNT; i++)
    {
        if (in_box(x, y, TOOL_X(i), TOOL_Y, TOOL_W, TOOL_H))
        {
            if (P.tool != i)
            {
                P.tool = i;
                P.drawing = 0;
                inv_ui(w);
            }

            return;
        }
    }

    if (in_box(x, y, MINUS_X, TOOL_Y, SM_W, TOOL_H))
    {
        set_size(w, P.size - 1);
        return;
    }

    if (in_box(x, y, PLUS_X, TOOL_Y, SM_W, TOOL_H))
    {
        set_size(w, P.size + 1);
        return;
    }

    if (in_box(x, y, CLEAR_X, TOOL_Y, CLEAR_W, TOOL_H))
    {
        cv_clear();
        P.drawing = 0;
        gui_window_invalidate_client(w);
        return;
    }
}

static void palette_click(gui_window_t *w, int x, int y)
{
    for (int i = 0; i < 16; i++)
    {
        if (in_box(x, y, pal_x(i) - 2, pal_y(i) - 2, 26, 26))
        {
            P.color = palette[i];

            if (P.tool == T_ERASER)
                P.tool = T_PENCIL;

            P.drawing = 0;

            inv_ui(w);

            return;
        }
    }
}

static void paint_click(gui_window_t *w, int x, int y, void *ctx)
{
    (void)ctx;

    P.stroking = 0;
    P.drawing = 0;

    if (y < TB_H)
    {
        toolbar_click(w, x, y);
        return;
    }

    if (x < PAL_W)
    {
        palette_click(w, x, y);
        return;
    }

    int px = pclamp(x - PAL_W, 0, CV_W - 1);
    int py = pclamp(y - TB_H,  0, CV_H - 1);

    if (is_shape())
    {
        /* початок фігури; кінець задається рухом миші, фіксація — при відпусканні */
        P.sx = P.ex = px;
        P.sy = P.ey = py;

        P.drawing = 1;

        inv_shape(w);

        return;
    }

    int size = brush_size();

    cv_fill(px - size / 2, py - size / 2, size, size, brush_color());

    inv_seg(w, px, py, px, py, size);

    P.lx = px;
    P.ly = py;

    P.stroking = 1;
}

static void paint_drag(gui_window_t *w, int x, int y, void *ctx)
{
    (void)ctx;

    int px = pclamp(x - PAL_W, 0, CV_W - 1);
    int py = pclamp(y - TB_H,  0, CV_H - 1);

    if (P.stroking)
    {
        int size = brush_size();

        cv_line(P.lx, P.ly, px, py, size, brush_color());

        inv_seg(w, P.lx, P.ly, px, py, size);

        P.lx = px;
        P.ly = py;

        return;
    }

    if (P.drawing)
    {
        inv_shape(w);       /* стара рамка */

        P.ex = px;
        P.ey = py;

        inv_shape(w);       /* нова рамка */
    }
}

static void paint_release(gui_window_t *w, int x, int y, void *ctx)
{
    (void)ctx;

    if (P.stroking)
    {
        P.stroking = 0;
        return;
    }

    if (P.drawing)
    {
        P.ex = pclamp(x - PAL_W, 0, CV_W - 1);
        P.ey = pclamp(y - TB_H,  0, CV_H - 1);

        shape_draw(cv_fill);

        P.drawing = 0;

        inv_shape(w);
    }
}

static void paint_wheel(gui_window_t *w, int delta, void *ctx)
{
    (void)ctx;

    if (delta > 0)
        set_size(w, P.size + 1);
    else if (delta < 0)
        set_size(w, P.size - 1);
}

static void paint_key(gui_window_t *w, const gui_key_t *k, void *ctx)
{
    (void)ctx;

    switch (k->code)
    {
        case '[':
            set_size(w, P.size - 1);
            break;

        case ']':
            set_size(w, P.size + 1);
            break;

        case '1':
        case '2':
        case '3':
        case '4':
            P.tool = k->code - '1';
            P.drawing = 0;
            P.stroking = 0;
            inv_ui(w);
            break;

        default:
            break;
    }
}

static void paint_close(gui_window_t *w, void *ctx)
{
    (void)w;
    (void)ctx;

    pwin = 0;

    P.drawing = 0;
    P.stroking = 0;
}

static const gui_ops_t paint_ops =
{
    .paint     = paint_cb,
    .resize    = 0,
    .key       = paint_key,
    .click     = paint_click,
    .wheel     = paint_wheel,
    .close     = paint_close,
    .can_close = 0,
    .drag      = paint_drag,
    .release   = paint_release
};

void paint_open(void)
{
    if (pwin)
    {
        gui_window_focus(pwin);
        return;
    }

    cv_clear();

    P.tool = T_PENCIL;
    P.size = 2;
    P.color = C_BLACK;

    P.drawing = 0;
    P.stroking = 0;

    P.lx = 0;
    P.ly = 0;

    P.sx = 0;
    P.sy = 0;

    P.ex = 0;
    P.ey = 0;

    pwin = gui_window_create(80, 80, 760, 520, "Merden Paint");

    if (!pwin)
        return;

    gui_window_set_resizable(pwin, 1);

    gui_window_set_ops(pwin, &paint_ops, 0);
}