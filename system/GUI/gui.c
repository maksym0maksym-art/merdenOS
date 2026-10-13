#include <stdint.h>
#include "gui.h"
#include "../../kernel/headers/mouse.h"
#include "../../kernel/headers/framebuffer.h"
#include "../../kernel/headers/keyboard.h"
#include "../../kernel/headers/rtc.h"
#include "../../kernel/headers/i2c_dw.h"      /* i2c_now / i2c_cycles_per_ms — годинник для анімацій */
#include "../../kernel/fonts/fonts.h"
#include "../../kernel/code/drivers/io.h"

/* жест трьома пальцями з touchpad.c: -1 — пальці пішли ліворуч, +1 — праворуч, 0 — нічого */
extern int tp_swipe_pop(void);


#define GUI_MAX_W 1920
#define GUI_MAX_H 1080
#define BORDER    2            /* товщина золотої рамки */
#define WIN_R     8            /* радіус заокруглення вікон */
#define MIN_W     220
#define MIN_H     100
#define GRIP      16
#define TASK_X0   100
#define TASK_W    140

/* ---- анімації вікон (як у Hyprland: плавне зсування/розтягування та поява) ---- */
#ifndef GUI_ANIMATIONS
#define GUI_ANIMATIONS 1       /* 0 — вимкнути всі анімації */
#endif
#ifndef GUI_ANIM_MS
#define GUI_ANIM_MS 170        /* тривалість, мс */
#endif
/* 1 — після Alt+Shift+N переходити слідом за вікном, 0 — вікно просто зникає на стіл N */
#ifndef GUI_WS_MOVE_FOLLOW
#define GUI_WS_MOVE_FOLLOW 0
#endif
#define WS_CELL_W 24           /* ширина клітинки цифри робочого столу на панелі */

/* слайд при перемиканні робочих столів, мс (0 — без анімації) */
#ifndef GUI_WS_ANIM_MS
#define GUI_WS_ANIM_MS 240
#endif
/* 0: пальці ліворуч -> наступний стіл (контент їде за пальцями, як на macOS); 1 — навпаки */
#ifndef GUI_WS_SWIPE_INVERT
#define GUI_WS_SWIPE_INVERT 0
#endif

/* фокус іде за мишею: вікно під курсором стає активним, щойно мишу посунули */
#ifndef GUI_FOCUS_FOLLOWS_MOUSE
#define GUI_FOCUS_FOLLOWS_MOUSE 1
#endif

/* на скільки пікселів треба потягнути тайлове вікно за заголовок, щоб почалось перетягування */
#ifndef GUI_DRAG_FLOAT_PX
#define GUI_DRAG_FLOAT_PX 12
#endif

#define C_DESKTOP    0x1E3A5F
#define C_TASKBAR    0x1B1F2A
#define C_TASKLINE   0x3A4050
#define C_TASKTEXT   0xC8C8C8
#define C_BORDER     0xD4AF37      /* золото — активне вікно */
#define C_BORDER_OFF 0x8A7425      /* темніше золото — неактивне */
#define C_TITLE_ON   0x000000
#define C_TITLE_OFF  0x6B7280
#define C_TITLETEXT  0xFFFFFF
#define C_CLIENT     0xEDEDED
#define C_CLOSE      0xC0392B
#define C_TBTN       0x3C5FA8
#define C_BTN_FACE   0xD4D0C8
#define C_BTN_LIGHT  0xFFFFFF
#define C_BTN_DARK   0x808080
#define C_BTN_TEXT   0x000000

#define START_ICON_W 28
#define START_ICON_H 16
#define START_ICON_SCALE 2

/* 1 — панель завдань зверху, 0 — знизу */
#define TASKBAR_TOP 1

#define TB_PAD      3
#define TB_BTN_H    36
#define START_X     6

static const char start_icon[START_ICON_H][START_ICON_W + 1] = {
    "              ..          ",
    "        .......           ",
    "      #+=##@#             ",
    "    ##*####               ",
    "   ##%####%         ---   ",
    "  ***####@  ##    ####==  ",
    "  **###################=* ",
    "     @######%%##########=+",
    "        @##    #@ ######+=",
    "       ###########   ###+*",
    "           ####  %@   @#++",
    "*#*#@   #######        #+*",
    " ***###########        ++ ",
    "   *+++#########       *  ",
    "       +++++++++          "
};

//палітра для малювання логотипу 
static uint32_t start_icon_color(char c)
{
    switch (c) 
    {
        case '.': return 0xEBEBEB;
        case '+': return 0xBEBEBE;
        case '=': return 0x969696;
        case '*': return 0x737373; 
        case '#': return 0x4B4B4B;
        case '%': return 0x2D2D2D;
        case '@': return 0x141414;
        case '-': return 0xD2D2D2;

        default:  return 0;
    } 
}

static void gfx_fill(int x, int y, int w, int h, uint32_t color); 
static void gfx_span_a(int x, int y, int w, uint32_t c, int a);

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    uint8_t  used;
    int      x, y, w, h;
    const char *text;
    uint32_t color;
    gui_click_fn fn;
    void    *ctx;
} widget_t;

struct gui_window {
    int  used;
    int icon;
    int  x, y, w, h;               /* геометрія, яка зараз МАЛЮЄТЬСЯ (під час анімації — проміжна) */
    const char *title;
    int  minimized, maximized, resizable;
    int  rx, ry, rw, rh;
    int  floating;                 /* 1 — поза тайлінгом */
    int  alpha;                    /* 255 — непрозоре, менше — напівпрозора клієнтська область */
    int  fx, fy, fw, fh;           /* геометрія для плаваючого режиму */

    /* анімація: з (sx,sy,sw,sh) у (tx,ty,tw,th) */
    int  anim;                     /* 1 — вікно зараз їде */
    int  anim_new;                 /* 1 — наступне розміщення це поява (росте з центру слота) */
    int  pend_rs;                  /* розмір змінився — сповістити вікно, коли анімація закінчиться */
    int  dragging;                 /* 1 — тайлове вікно зараз тягнуть мишею (воно вільно йде за курсором) */
    int  ws;                       /* номер робочого столу, на якому живе вікно */
    int  sx, sy, sw, sh;
    int  tx, ty, tw, th;
    uint64_t at0;
    const gui_ops_t *ops;
    void *ctx;
    int  nwidgets;
    widget_t widgets[GUI_MAX_WIDGETS];
};

#define W_BUTTON 1
#define W_LABEL  2

static uint32_t back[GUI_MAX_W * GUI_MAX_H];
static uint32_t wall[GUI_MAX_W * GUI_MAX_H];
static int      xmap[GUI_MAX_W];
static int      have_wall;
static int      W, H;

static rect_t clip;

#define MAX_RECTS 8
static rect_t rr[MAX_RECTS]; static int nrr;
static rect_t fr[MAX_RECTS]; static int nfr;

#ifdef GUI_STATS
static unsigned long stat_render_px, stat_flip_px;
#endif

static void flush(void);

static gui_window_t pool[GUI_MAX_WINDOWS];
static gui_window_t *order[GUI_MAX_WINDOWS];   /* z-порядок */
static int norder;

static gui_window_t *tiles[GUI_MAX_WINDOWS];   /* порядок тайлінгу (за створенням) */
static int ntiles;

static gui_window_t *drag_win;
static gui_window_t *cap_win;      /* вікно, що «захопило» мишу після натискання */
static int cap_lx, cap_ly;
static int drag_dx, drag_dy;
static gui_window_t *drop_target;  /* тайлове вікно під курсором під час перетягування (підсвічується) */
static int fm_x = -1, fm_y = -1;   /* остання позиція миші (для фокусу за мишею) */
static int drag_px, drag_py;       /* де натиснули на заголовок (для порогу «стати плаваючим») */
static gui_window_t *size_win;
static int size_mx, size_my, size_w, size_h;
static gui_window_t *press_win;
static widget_t *press_btn;

static int  quit_flag;
static void (*launcher)(void);
static int start_open;

static char     clock_txt[8];
static uint32_t ticks;

static int k_shift, k_ctrl, k_alt, k_caps, k_e0;
static int k_super, k_super_used;      /* Win/Super: затиснута; чи натискали щось, поки вона була затиснута */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

/* вікно бере участь у тайлінгу? */
static int win_tiled(const gui_window_t *w) { return GUI_TILING && !w->floating; }

static int cur_ws;                       /* активний робочий стіл */
/* вікно видиме: існує, не згорнуте і лежить на поточному столі */
static int win_visible(const gui_window_t *w) { return w->used && !w->minimized && w->ws == cur_ws; }

/* слайд між столами: cur_ws уже новий, ws_from — звідки їдемо */
static int      ws_anim;        /* 1 — йде слайд */
static int      ws_from;
static int      ws_dir;         /* +1: новий стіл в'їжджає справа, -1: зліва */
static int      ws_off;         /* на скільки пікселів уже проїхали (0..W) */
static uint64_t ws_t0;

/* ---------------- Геометрія: панель завдань і робочий стіл ---------------- */
static int taskbar_y(void)  { return TASKBAR_TOP ? 0 : H - GUI_TASKBAR_H; }
static int desk_y0(void)    { return TASKBAR_TOP ? GUI_TASKBAR_H : 0; }
static int desk_y1(void)    { return TASKBAR_TOP ? H : H - GUI_TASKBAR_H; }
static int desk_h(void)     { return desk_y1() - desk_y0(); }

static int rect_clip_screen(rect_t *r) {
    int x0 = imax(r->x, 0), y0 = imax(r->y, 0);
    int x1 = imin(r->x + r->w, W), y1 = imin(r->y + r->h, H);
    if (x1 <= x0 || y1 <= y0) return 0;
    r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
    return 1;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int rect_overlap(const rect_t *a, const rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void rect_union(rect_t *a, const rect_t *b) {
    int x0 = imin(a->x, b->x), y0 = imin(a->y, b->y);
    int x1 = imax(a->x + a->w, b->x + b->w), y1 = imax(a->y + a->h, b->y + b->h);
    a->x = x0; a->y = y0; a->w = x1 - x0; a->h = y1 - y0;
}

static void list_add(rect_t *list, int *n, rect_t r) {
    for (;;) {
        int merged = 0;
        for (int i = 0; i < *n; i++) {
            if (rect_overlap(&list[i], &r)) {
                rect_union(&r, &list[i]);
                list[i] = list[--(*n)];
                merged = 1;
                break;
            }
        }
        if (!merged) break;
    }
    if (*n >= MAX_RECTS) {
        for (int i = 0; i < *n; i++) rect_union(&r, &list[i]);
        *n = 0;
    }
    list[(*n)++] = r;
}

void gui_invalidate_rect(int x, int y, int w, int h) {
    rect_t r = { x, y, w, h };
    if (!rect_clip_screen(&r)) return;
    list_add(rr, &nrr, r);
}

static void add_render_minus(rect_t a, rect_t hole) {
    if (!rect_overlap(&a, &hole)) { list_add(rr, &nrr, a); return; }
    int ax1 = a.x + a.w, ay1 = a.y + a.h;
    int hx0 = imax(a.x, hole.x), hy0 = imax(a.y, hole.y);
    int hx1 = imin(ax1, hole.x + hole.w), hy1 = imin(ay1, hole.y + hole.h);
    if (hy0 > a.y)  { rect_t q = { a.x, a.y, a.w, hy0 - a.y };       list_add(rr, &nrr, q); }
    if (hy1 < ay1)  { rect_t q = { a.x, hy1, a.w, ay1 - hy1 };       list_add(rr, &nrr, q); }
    if (hx0 > a.x)  { rect_t q = { a.x, hy0, hx0 - a.x, hy1 - hy0 }; list_add(rr, &nrr, q); }
    if (hx1 < ax1)  { rect_t q = { hx1, hy0, ax1 - hx1, hy1 - hy0 }; list_add(rr, &nrr, q); }
}

void gui_invalidate_all(void) { gui_invalidate_rect(0, 0, W, H); }

void gui_window_invalidate(gui_window_t *win) {
    if (win && win->used) gui_invalidate_rect(win->x, win->y, win->w, win->h);
}

void gui_window_client(const gui_window_t *win, int *x, int *y, int *cw, int *ch) {
    if (x)  *x  = win->x + BORDER;
    if (y)  *y  = win->y + BORDER + GUI_TITLE_H;
    if (cw) *cw = win->w - 2 * BORDER;
    if (ch) *ch = win->h - 2 * BORDER - GUI_TITLE_H;
}

void gui_window_invalidate_client(gui_window_t *win) {
    if (!win || !win_visible(win)) return;
    int x, y, cw, ch;
    gui_window_client(win, &x, &y, &cw, &ch);
    gui_invalidate_rect(x, y, cw, ch);
}

static void inv_taskbar(void) { gui_invalidate_rect(0, taskbar_y(), W, GUI_TASKBAR_H); }

static inline void fill_row(uint32_t *d, uint32_t c, int n) {
    __asm__ volatile ("cld; rep stosl" : "+D"(d), "+c"(n) : "a"(c) : "memory");
}

static void gfx_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(x, clip.x), y0 = imax(y, clip.y);
    int x1 = imin(x + w, clip.x + clip.w), y1 = imin(y + h, clip.y + clip.h);
    if (x1 <= x0 || y1 <= y0) return;
    for (int py = y0; py < y1; py++) fill_row(&back[py * W + x0], c, x1 - x0);
}

static const char rose_px[18][17] = {
    "                ",
    "     rrrrrr     ",
    "   rrRRRRRRrr   ",
    "  rRRhhRRRRRRr  ",
    "  rRhRRrrRRRRr  ",
    "  rRRRrRRrRRRr  ",
    "  rRRRrRhRrRRr  ",
    "  rRRRRrrRRRRr  ",
    "   rRRRRRRRRr   ",
    "    rrRRRRrr    ",
    "      rggr      ",
    "       gg       ",
    "   gG  gg  Gg   ",
    "  gGGG gg GGGg  ",
    "  gGGGGggGGGGg  ",
    "   gGGGggGGGg   ",
    "     ggggg      ",
    "       gg       ",
};

static uint32_t rose_color(char c) {
    switch (c) {
    case 'R': return 0xD81E2B;       /* пелюстки */
    case 'r': return 0x8E0F1C;       /* тіні й контур */
    case 'h': return 0xF26A74;       /* відблиск */
    case 'G': return 0x2FA045;       /* листя */
    case 'g': return 0x1B642A;       /* стебло */
    default:  return 0;
    }
}

/* ліва (half = 0) або права (half = 1) половина троянди; фон не чіпаємо */
static void gfx_rose_half(int x, int y, int half) {
    if (x + 8 <= clip.x || x >= clip.x + clip.w || y + 18 <= clip.y || y >= clip.y + clip.h) return;
    for (int row = 0; row < 18; row++)
        for (int col = 0; col < 8; col++) {
            char c = rose_px[row][half * 8 + col];
            if (c != ' ') gfx_fill(x + col, y + row, 1, 1, rose_color(c));
        }
}

/* ┌ ─ └ : лінія товщиною 2 px по центру клітинки 8x18; сусідні клітинки зростаються без щілин */
static void gfx_box_glyph(int x, int y, char ch, uint32_t fg) {
    if (x + 8 <= clip.x || x >= clip.x + clip.w || y + 18 <= clip.y || y >= clip.y + clip.h) return;
    if (ch == GUI_CH_BOX_H) {
        gfx_fill(x, y + 8, 8, 2, fg);                         /* ─ */
    } else if (ch == GUI_CH_BOX_TL) {
        gfx_fill(x + 3, y + 8, 5, 2, fg);                     /* ┌ : вправо ... */
        gfx_fill(x + 3, y + 8, 2, 10, fg);                    /*     ... і вниз до краю клітинки */
    } else {
        gfx_fill(x + 3, y,     2, 10, fg);                    /* └ : вгору від краю клітинки ... */
        gfx_fill(x + 3, y + 8, 5, 2, fg);                     /*     ... і вправо */
    }
}

static void gfx_char(int x, int y, char ch, uint32_t fg) {
    if (ch == GUI_CH_ROSE_L) { gfx_rose_half(x, y, 0); return; }     /* кольорова троянда: колір fg ігнорується */
    if (ch == GUI_CH_ROSE_R) { gfx_rose_half(x, y, 1); return; }
    if (ch == GUI_CH_BOX_TL || ch == GUI_CH_BOX_H || ch == GUI_CH_BOX_BL) { gfx_box_glyph(x, y, ch, fg); return; }
    if (x + 8 <= clip.x || x >= clip.x + clip.w || y + 18 <= clip.y || y >= clip.y + clip.h) return;
    const uint8_t *g = font8x18[(uint8_t)ch];
    int r0 = imax(0, clip.y - y), r1 = imin(18, clip.y + clip.h - y);
    int inside = (x >= clip.x && x + 8 <= clip.x + clip.w);

    for (int row = r0; row < r1; row++) {
        uint8_t bits = g[row];
        if (!bits) continue;
        uint32_t *p = &back[(y + row) * W + x];
        if (inside) {
            if (bits & 0x80) p[0] = fg;
            if (bits & 0x40) p[1] = fg;
            if (bits & 0x20) p[2] = fg;
            if (bits & 0x10) p[3] = fg;
            if (bits & 0x08) p[4] = fg;
            if (bits & 0x04) p[5] = fg;
            if (bits & 0x02) p[6] = fg;
            if (bits & 0x01) p[7] = fg;
        } else {
            for (int col = 0; col < 8; col++) {
                int px = x + col;
                if (px < clip.x || px >= clip.x + clip.w) continue;
                if (bits & (0x80 >> col)) back[(y + row) * W + px] = fg;
            }
        }
    }
}

static int gui_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void gfx_text(int x, int y, const char *s, uint32_t fg) {
    for (; *s; s++, x += 8) gfx_char(x, y, *s, fg);
}

static void gfx_bevel(int x, int y, int w, int h, uint32_t tl, uint32_t br) {
    gfx_fill(x, y, w, 1, tl);
    gfx_fill(x, y, 1, h, tl);
    gfx_fill(x, y + h - 1, w, 1, br);
    gfx_fill(x + w - 1, y, 1, h, br);
}

void gui_draw_rect(int x, int y, int w, int h, uint32_t color)   { gfx_fill(x, y, w, h, color); }
void gui_get_clip(int *x, int *y, int *w, int *h) {
    if (x) *x = clip.x;
    if (y) *y = clip.y;
    if (w) *w = clip.w;
    if (h) *h = clip.h;
}
void gui_draw_char(int x, int y, char ch, uint32_t color)        { gfx_char(x, y, ch, color); }
void gui_draw_text(int x, int y, const char *s, uint32_t color)  { gfx_text(x, y, s, color); }

static inline void copy_row(uint32_t *d, const uint32_t *s, int n) {
    __asm__ volatile ("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
}

/* шпалери малюються лише на робочому столі (поза панеллю завдань) */
static void gfx_blit_wall(void) {
    int x0 = clip.x, x1 = clip.x + clip.w;
    int y0 = imax(clip.y, desk_y0()), y1 = imin(clip.y + clip.h, desk_y1());
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++)
        copy_row(&back[y * W + x0], &wall[y * W + x0], x1 - x0);
}

static gui_window_t *active_window(void) {
    for (int i = norder - 1; i >= 0; i--)
        if (win_visible(order[i])) return order[i];
    return 0;
}

/* Розмір клієнтської області, який вікно матиме ПІСЛЯ анімації (програми мають верстатись під нього,
 * а не під проміжні кадри). */
static void win_client_size(const gui_window_t *win, int *cw, int *ch) {
    int w = win->anim ? win->tw : win->w;
    int h = win->anim ? win->th : win->h;
    *cw = w - 2 * BORDER;
    *ch = h - 2 * BORDER - GUI_TITLE_H;
}

static void notify_resize(gui_window_t *win) {
    if (win->ops && win->ops->resize) {
        int cw, ch;
        win_client_size(win, &cw, &ch);
        win->ops->resize(win, cw, ch, win->ctx);
    }
}

static void reset_interaction(gui_window_t *win) {
    win->dragging = 0;
    if (drop_target) { gui_window_invalidate(drop_target); drop_target = 0; }
    if (drag_win == win) drag_win = 0;
    if (size_win == win) size_win = 0;
    if (cap_win == win) cap_win = 0;
    if (press_win == win) { press_win = 0; press_btn = 0; }
}

/* ---------------- Анімації ----------------
 * Усе, що не знає про анімацію (малювання, миша, панель завдань), бачить звичайні w->x/y/w/h —
 * це поточний кадр. Мета лежить у tx/ty/tw/th. Час береться з TSC (без ділення 64-бітних). */

static uint64_t anim_now(void) { return i2c_now(); }

/* прогрес 0..256 від моменту t0 */
static int anim_progress_ms(uint64_t t0, int ms) {
    uint32_t cpms = i2c_cycles_per_ms();
    if (!cpms) cpms = 2000000u;
    uint64_t dur = (uint64_t)ms * cpms;
    uint64_t e = anim_now() - t0;
    if (dur == 0 || e >= dur) return 256;
    while (dur > 0xFFFFFull) { dur >>= 1; e >>= 1; }       /* вміщаємо в 32 біти — без __udivdi3 */
    if (dur == 0) return 256;
    return (int)(((uint32_t)e * 256u) / (uint32_t)dur);
}

static int anim_progress(uint64_t t0) { return anim_progress_ms(t0, GUI_ANIM_MS); }

/* ease-out cubic: швидкий старт, м'яка зупинка */
static int anim_ease(int p) {
    int q = 256 - p;
    return 256 - (q * q * q) / 65536;
}

static int anim_lerp(int a, int b, int e) { return a + (((b - a) * e) >> 8); }

/* один крок усіх анімацій; викликається раз на кадр */
static void anim_step(void) {
    if (ws_anim) {                                           /* слайд робочих столів */
        int p = anim_progress_ms(ws_t0, GUI_WS_ANIM_MS);
        if (p >= 256) { ws_anim = 0; ws_off = 0; }
        else          ws_off = (W * anim_ease(p)) >> 8;
        gui_invalidate_rect(0, desk_y0(), W, desk_h());      /* увесь стіл змінюється з кожним кадром */
    }

    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_window_t *w = &pool[i];
        if (!w->used || !w->anim) continue;

        int p = anim_progress(w->at0);
        int nx, ny, nw, nh;
        if (p >= 256) {
            nx = w->tx; ny = w->ty; nw = w->tw; nh = w->th;
        } else {
            int e = anim_ease(p);
            nx = anim_lerp(w->sx, w->tx, e);
            ny = anim_lerp(w->sy, w->ty, e);
            nw = anim_lerp(w->sw, w->tw, e);
            nh = anim_lerp(w->sh, w->th, e);
        }

        if (nx != w->x || ny != w->y || nw != w->w || nh != w->h) {
            gui_window_invalidate(w);                        /* старий кадр */
            w->x = nx; w->y = ny; w->w = nw; w->h = nh;
            gui_window_invalidate(w);                        /* новий кадр */
        }

        if (p >= 256) {
            w->anim = 0;
            if (w->pend_rs) { w->pend_rs = 0; notify_resize(w); }
        }
    }
}

/* ---------------- Тайлінг (dwindle, як у Hyprland) ----------------
 * Область робочого столу зі зовнішнім відступом ділиться так:
 *   вікно 0 — ліва половина, решта — права;
 *   вікно 1 — верхня половина решти, решта — нижня;
 *   вікно 2 — ліва половина нової решти ... і так по спіралі.
 * Останнє вікно займає всю область, що лишилась. Між вікнами — GUI_TILE_GAP. */

static void tile_set(gui_window_t *w, int x, int y, int ww, int hh) {
    int isnew = w->anim_new;
    w->anim_new = 0;
    if (w->dragging) return;                 /* слот зарезервовано, але вікно зараз веде користувач */

    /* поточна ціль: якщо вікно їде — його мета, інакше те, що намальовано */
    int cx = w->anim ? w->tx : w->x, cy = w->anim ? w->ty : w->y;
    int cw = w->anim ? w->tw : w->w, ch = w->anim ? w->th : w->h;
    if (!isnew && cx == x && cy == y && cw == ww && ch == hh) return;

    int resized = (cw != ww || ch != hh) || isnew;

    if (!GUI_ANIMATIONS || w->minimized || w->ws != cur_ws) {                  /* без анімації: миттєво, як раніше */
        gui_window_invalidate(w);
        w->anim = 0; w->pend_rs = 0;
        w->x = x; w->y = y; w->w = ww; w->h = hh;
        gui_window_invalidate(w);
        if (resized) notify_resize(w);
        return;
    }

    gui_window_invalidate(w);
    if (isnew) {                                            /* поява: росте з центру слота */
        int dx = ww / 14, dy = hh / 14;
        w->x = x + dx; w->y = y + dy; w->w = ww - 2 * dx; w->h = hh - 2 * dy;
    }
    w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
    w->tx = x;    w->ty = y;    w->tw = ww;   w->th = hh;
    w->at0 = anim_now();
    w->anim = 1;
    if (resized) w->pend_rs = 1;
    gui_window_invalidate(w);
}

static void tile_add(gui_window_t *w) {
    if (ntiles < GUI_MAX_WINDOWS) tiles[ntiles++] = w;
}

static void tile_remove(gui_window_t *w) {
    int j = 0;
    for (int i = 0; i < ntiles; i++)
        if (tiles[i] != w) tiles[j++] = tiles[i];
    ntiles = j;
}

static void tile_layout_ws(int ws) {
    if (!GUI_TILING) return;

    gui_window_t *list[GUI_MAX_WINDOWS];
    int n = 0, j = 0;
    for (int i = 0; i < ntiles; i++) {
        gui_window_t *w = tiles[i];
        if (!w->used) continue;
        tiles[j++] = w;
        if (w->minimized || w->floating || w->ws != ws) continue;
        list[n++] = w;
    }
    ntiles = j;

    int ax = GUI_TILE_OUT, ay = desk_y0() + GUI_TILE_OUT;
    int aw = W - 2 * GUI_TILE_OUT, ah = desk_h() - 2 * GUI_TILE_OUT;
    if (aw < MIN_W) aw = MIN_W;
    if (ah < MIN_H) ah = MIN_H;
    int fx = ax, fy = ay, fw = aw, fh = ah;      /* повна область (для розгорнутого вікна) */

    int nm = 0;
    for (int i = 0; i < n; i++) if (!list[i]->maximized) nm++;

    int k = 0;
    for (int i = 0; i < n; i++) {
        gui_window_t *w = list[i];
        if (w->maximized) { tile_set(w, fx, fy, fw, fh); continue; }

        if (k == nm - 1) {
            tile_set(w, ax, ay, aw, ah);
        } else if ((k & 1) == 0) {
            int lw = (aw - GUI_TILE_GAP) / 2;
            if (lw < 1) lw = 1;
            tile_set(w, ax, ay, lw, ah);
            ax += lw + GUI_TILE_GAP;
            aw -= lw + GUI_TILE_GAP;
        } else {
            int th = (ah - GUI_TILE_GAP) / 2;
            if (th < 1) th = 1;
            tile_set(w, ax, ay, aw, th);
            ay += th + GUI_TILE_GAP;
            ah -= th + GUI_TILE_GAP;
        }
        k++;
    }
}

static void tile_layout(void) { tile_layout_ws(cur_ws); }

static void tile_swap(gui_window_t *a, gui_window_t *b) {
    int ia = -1, ib = -1;
    for (int i = 0; i < ntiles; i++) {
        if (tiles[i] == a) ia = i;
        if (tiles[i] == b) ib = i;
    }
    if (ia < 0 || ib < 0) return;
    tiles[ia] = b;
    tiles[ib] = a;
    tile_layout();
}

/* тайлове вікно під точкою (крім exclude) */
static gui_window_t *tile_at(int x, int y, const gui_window_t *exclude) {
    for (int i = norder - 1; i >= 0; i--) {
        gui_window_t *w = order[i];
        if (w == exclude || !win_visible(w) || !win_tiled(w)) continue;
        if (in_rect(x, y, w->x, w->y, w->w, w->h)) return w;
    }
    return 0;
}

/* вікно, яке тягнуть: рухаємо вільно, а підсвічуємо слот-ціль */
static void tile_drag_to(gui_window_t *win, int x, int y, int mx, int my) {
    int y0 = desk_y0(), y1 = desk_y1();
    if (x < 40 - win->w) x = 40 - win->w;
    if (x > W - 40) x = W - 40;
    if (y < y0) y = y0;
    if (y > y1 - GUI_TITLE_H) y = y1 - GUI_TITLE_H;
    if (x != win->x || y != win->y) {
        gui_window_invalidate(win);
        win->x = x; win->y = y;
        gui_window_invalidate(win);
    }
    gui_window_t *t = tile_at(mx, my, win);
    if (t != drop_target) {
        if (drop_target) gui_window_invalidate(drop_target);
        drop_target = t;
        if (t) gui_window_invalidate(t);
    }
}

int gui_workspace_current(void) { return cur_ws; }

void gui_workspace_switch(int ws) {
    if (ws < 0 || ws >= GUI_WORKSPACES || ws == cur_ws) return;

    /* обриваємо все, що залежало від поточного столу */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) pool[i].dragging = 0;
    drag_win = size_win = cap_win = 0;
    press_win = 0; press_btn = 0;
    drop_target = 0;

    int from = cur_ws;
    cur_ws = ws;
    tile_layout();                           /* на випадок, якщо вікна столу ще не розкладені */

    if (GUI_ANIMATIONS && GUI_WS_ANIM_MS > 0) {
        ws_from = from;
        ws_dir = ws > from ? 1 : -1;
        ws_off = 0;
        ws_t0 = anim_now();
        ws_anim = 1;
    } else {
        ws_anim = 0;
    }
    gui_invalidate_all();
}

void gui_workspace_step(int dir) {
    gui_workspace_switch(cur_ws + (dir > 0 ? 1 : -1));       /* поза межами 0..N-1 gui_workspace_switch ігнорує */
}

void gui_window_move_to_workspace(gui_window_t *win, int ws) {
    if (!win || !win->used || ws < 0 || ws >= GUI_WORKSPACES || win->ws == ws) return;
    int old = win->ws;

    gui_window_invalidate(win);
    reset_interaction(win);
    win->ws = ws;
    tile_layout_ws(old);                     /* старий стіл переверстується без цього вікна */
    tile_layout_ws(ws);                      /* а на новому воно стає в свій слот */

    if (GUI_WS_MOVE_FOLLOW) {
        gui_workspace_switch(ws);
        gui_window_focus(win);
    } else {
        gui_window_t *a = active_window();
        if (a) gui_window_invalidate(a);
    }
    inv_taskbar();
}

void gui_window_focus(gui_window_t *win) {
    if (!win || !win->used) return;
    if (win->ws != cur_ws) gui_workspace_switch(win->ws);   /* вікно з іншого столу — спершу йдемо туди */
    gui_window_t *prev = active_window();
    if (win->minimized) {
        win->minimized = 0;
        if (win_tiled(win)) win->anim_new = 1;               /* розгортається з панелі так само, як поява */
        gui_window_invalidate(win);
        if (win_tiled(win)) tile_layout();
        win->anim_new = 0;
    }

    int i = 0;
    while (i < norder && order[i] != win) i++;
    if (i == norder) return;
    for (; i < norder - 1; i++) order[i] = order[i + 1];
    order[norder - 1] = win;

    if (prev && prev != win) gui_window_invalidate(prev);
    gui_window_invalidate(win);
    inv_taskbar();
}

void gui_window_minimize(gui_window_t *win) {
    if (!win || !win->used || win->minimized) return;
    gui_window_invalidate(win);
    win->minimized = 1;
    win->anim = 0; win->pend_rs = 0;                         /* згорнуте вікно не анімується */
    reset_interaction(win);
    if (win_tiled(win)) tile_layout();
    gui_window_t *a = active_window();
    if (a) gui_window_invalidate(a);
    inv_taskbar();
}

void gui_window_resize(gui_window_t *win, int w, int h) {
    if (!win || !win->used) return;
    if (win_tiled(win)) return;              /* розмір задає тайлінг */
    int max_w = W, max_h = desk_h();
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    if (w == win->w && h == win->h) return;
    gui_window_invalidate(win);
    win->w = w; win->h = h;
    gui_window_invalidate(win);
    notify_resize(win);
}

void gui_window_toggle_maximize(gui_window_t *win) {
    if (!win || !win->used) return;

    if (win_tiled(win)) {                    /* у тайлінгу «розгорнути» = на всю область з відступами */
        win->maximized = !win->maximized;
        tile_layout();
        gui_window_invalidate(win);
        return;
    }

    gui_window_invalidate(win);
    if (!win->maximized) {
        win->rx = win->x; win->ry = win->y; win->rw = win->w; win->rh = win->h;
        win->x = 0; win->y = desk_y0(); win->w = W; win->h = desk_h();
        win->maximized = 1;
    } else {
        win->x = win->rx; win->y = win->ry; win->w = win->rw; win->h = win->rh;
        win->maximized = 0;
    }
    gui_window_invalidate(win);
    notify_resize(win);
}

void gui_window_set_resizable(gui_window_t *win, int resizable) {
    if (win) win->resizable = resizable ? 1 : 0;
}

void gui_window_set_floating(gui_window_t *win, int floating) {
    if (!win || !win->used) return;
    floating = floating ? 1 : 0;
    if (win->floating == floating) return;

    if (floating) {                          /* тайл -> плаваюче */
        win->floating = 1;
        win->maximized = 0;
        win->dragging = 0;
        win->anim = 0; win->pend_rs = 0;     /* далі вікном керує користувач, а не анімація */
        gui_window_invalidate(win);
        int w = win->fw, h = win->fh;
        if (w < MIN_W) w = MIN_W;
        if (h < MIN_H) h = MIN_H;
        if (w > W) w = W;
        if (h > desk_h()) h = desk_h();
        int x = win->fx, y = win->fy;
        if (y < desk_y0()) y = desk_y0();
        if (y > desk_y1() - GUI_TITLE_H) y = desk_y1() - GUI_TITLE_H;
        if (x < 40 - w) x = 40 - w;
        if (x > W - 40) x = W - 40;
        win->x = x; win->y = y; win->w = w; win->h = h;
        gui_window_invalidate(win);
        notify_resize(win);
        tile_layout();
        gui_window_focus(win);
    } else {                                 /* плаваюче -> тайл */
        win->fx = win->x; win->fy = win->y; win->fw = win->w; win->fh = win->h;
        win->floating = 0;
        win->maximized = 0;
        tile_layout();                       /* вікно плавно їде зі свого місця в слот */
    }
}

void gui_window_toggle_floating(gui_window_t *win) {
    if (win) gui_window_set_floating(win, !win->floating);
}

void gui_window_set_ops(gui_window_t *win, const gui_ops_t *ops, void *ctx) {
    if (!win) return;
    win->ops = ops;
    win->ctx = ctx;
    gui_window_invalidate(win);
    if (win->used && win_tiled(win)) notify_resize(win);   /* розмір (кінцевий) уже задано тайлінгом */
}

void gui_set_launcher(void (*fn)(void)) { launcher = fn; }
void gui_quit(void) { quit_flag = 1; }
static int client_x(const gui_window_t *w) { return w->x + BORDER; }
static int client_y(const gui_window_t *w) { return w->y + BORDER + GUI_TITLE_H; }

static void draw_button(const gui_window_t *win, const widget_t *b)
{
    int x = client_x(win) + b->x;
    int y = client_y(win) + b->y;

    int pressed = (b == press_btn && win == press_win);

    /* GNOME-style: плоска кнопка без рамки */
    gfx_fill(x, y, b->w, b->h,
             pressed ? 0xD0D0D0 : C_BTN_FACE);

    int tw = 8 * gui_strlen(b->text);

    gfx_text(
        x + (b->w - tw) / 2,
        y + (b->h - 18) / 2,
        b->text,
        C_BTN_TEXT
    );
}

static void draw_grip(const gui_window_t *win) {
    int xr = win->x + win->w - BORDER - 2, yb = win->y + win->h - BORDER - 2;
    for (int d = 4; d <= 12; d += 4)
        for (int t = 0; t <= d; t++) {
            gfx_fill(xr - t,     yb - d + t,     1, 1, C_BTN_DARK);
            gfx_fill(xr - t - 1, yb - d + t,     1, 1, C_BTN_LIGHT);
        }
}

/* ---------------- Заокруглені вікна ---------------- */

/* скільки пікселів зрізати в рядку j кута радіуса r */
static int corner_inset(int r, int j) {
    int dy2 = 2 * (r - j) - 1;
    for (int in = 0; in < r; in++) {
        int dx2 = 2 * (r - in) - 1;
        if (dx2 * dx2 + dy2 * dy2 <= 4 * r * r) return in;
    }
    return r;
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t c, int top, int bottom) {
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (top && j < r)               in = corner_inset(r, j);
        else if (bottom && j >= h - r)  in = corner_inset(r, h - 1 - j);
        gfx_span_a(x + in, y + j, w - 2 * in, c, 256);
    }
}

static void draw_window(const gui_window_t *win, int active) {
    int R  = (win->maximized && !win_tiled(win)) ? 0 : WIN_R;   /* на весь екран — без заокруглення */
    int Ri = R > BORDER ? R - BORDER : 0;

    /* золота рамка (зовнішній заокруглений прямокутник) */
    fill_round(win->x, win->y, win->w, win->h, R, active ? C_BORDER : C_BORDER_OFF, 1, 1);

    /* заголовок: заокруглений зверху */
    fill_round(win->x + BORDER, win->y + BORDER, win->w - 2 * BORDER, GUI_TITLE_H,
               Ri, active ? C_TITLE_ON : C_TITLE_OFF, 1, 0);
    {
        rect_t saved = clip;
        int x0 = imax(win->x + BORDER, clip.x), y0 = imax(win->y + BORDER, clip.y);
        int x1 = imin(win->x + win->w - BORDER - 8, clip.x + clip.w);
        int y1 = imin(win->y + BORDER + GUI_TITLE_H, clip.y + clip.h);
        if (x1 > x0 && y1 > y0) {
            clip.x = x0; clip.y = y0; clip.w = x1 - x0; clip.h = y1 - y0;
            gfx_text(win->x + 10, win->y + BORDER + 3, win->title, C_TITLETEXT);
        }
        clip = saved;
    }

    /* клієнтська область: заокруглена знизу */
    int cx = client_x(win), cy = client_y(win);
    int cw = win->w - 2 * BORDER, ch = win->h - 2 * BORDER - GUI_TITLE_H;
    fill_round(cx, cy, cw, ch, Ri, C_CLIENT, 0, 1);

    rect_t saved = clip;
    int x0 = imax(cx, clip.x), y0 = imax(cy, clip.y);
    int x1 = imin(cx + cw, clip.x + clip.w), y1 = imin(cy + ch, clip.y + clip.h);
    if (x1 > x0 && y1 > y0) {
        clip.x = x0; clip.y = y0; clip.w = x1 - x0; clip.h = y1 - y0;
        if (win->ops && win->ops->paint) win->ops->paint((gui_window_t *)win, cx, cy, cw, ch, win->ctx);
        for (int i = 0; i < win->nwidgets; i++) {
            const widget_t *b = &win->widgets[i];
            if (b->used == W_LABEL) gfx_text(cx + b->x, cy + b->y, b->text, b->color);
            else if (b->used == W_BUTTON) draw_button(win, b);
        }
        if (win->resizable && !win->maximized && !win_tiled(win)) draw_grip(win);
    }
    clip = saved;
}

static int count_windows(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) if (pool[i].used) n++;
    return n;
}

static void draw_tbutton(int x, int y, int w, int h, const char *text, int pressed, uint32_t color) {
    gfx_fill(x, y, w, h, C_BTN_FACE);
    if (pressed) gfx_bevel(x, y, w, h, C_BTN_DARK, C_BTN_LIGHT);
    else         gfx_bevel(x, y, w, h, C_BTN_LIGHT, C_BTN_DARK);
    int n = gui_strlen(text);
    int maxc = (w - 8) / 8;
    char buf[40];
    if (n > maxc) n = maxc;
    if (n > 38) n = 38;
    for (int i = 0; i < n; i++) buf[i] = text[i];
    buf[n] = 0;
    gfx_text(x + (w - 8 * n) / 2 + pressed, y + (h - 18) / 2 + pressed, buf, color);
}

static int start_scale(void)
{
    return (GUI_TASKBAR_H - 2 * TB_PAD >= START_ICON_H * 2 + 4) ? 2 : 1;
}

static int start_btn_w(void) 
{
    return START_ICON_W * start_scale() + 16;
}

static int start_btn_h(void)
{
    return GUI_TASKBAR_H - 2 * TB_PAD;
}

static int start_btn_y(void)
{
    return taskbar_y() + TB_PAD;
}

static int task_x0(void)
{
    return START_X + start_btn_w() + 8;
}

static void draw_start_icon(int bx, int by, int bw, int bh)
{
    int s = start_scale();
    int x0 = bx + (bw - START_ICON_W * s) / 2;
    int y0 = by + (bh - START_ICON_H * s) / 2;

    for (int iy = 0; iy < START_ICON_H; iy++)
    {
        for(int iw = 0; iw < START_ICON_W; iw++)
        {
            char c = start_icon[iy][iw];
            if (c == ' '|| c == '\0')continue;

            gfx_fill(
                x0 + iw *s,
                y0 + iy *s, 
                s, 
                s, 
                start_icon_color(c)        
            );
        }
    }
}

static int task_btn_w(int count)
{
    if (count <= 0)
        return TASK_W;

    int w = 50;

    if (w < 40)
        w = 40;

    return w;
}

/* ---------------- Заокруглення, прозорість, масштабований текст ---------------- */
static const uint8_t rinset[4] = { 3, 2, 1, 1 };

/* горизонтальна смужка з альфою a (0..256; 256 — суцільний колір) */
static void gfx_span_a(int x, int y, int w, uint32_t c, int a) {
    if (y < clip.y || y >= clip.y + clip.h) return;
    int x0 = imax(x, clip.x), x1 = imin(x + w, clip.x + clip.w);
    if (x1 <= x0) return;
    uint32_t *p = &back[y * W + x0];
    int n = x1 - x0;
    if (a >= 256) { fill_row(p, c, n); return; }
    uint32_t cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
    uint32_t ua = (uint32_t)a, ia = (uint32_t)(256 - a);
    for (int i = 0; i < n; i++) {
        uint32_t q = p[i];
        uint32_t r = (cr * ua + ((q >> 16) & 255) * ia) >> 8;
        uint32_t g = (cg * ua + ((q >> 8) & 255) * ia) >> 8;
        uint32_t b = (cb * ua + (q & 255) * ia) >> 8;
        p[i] = (r << 16) | (g << 8) | b;
    }
}

static void gfx_rfill_a(int x, int y, int w, int h, uint32_t c, int a) {
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (w >= 8 && h >= 8) {
            if (j < 4)           in = rinset[j];
            else if (j >= h - 4) in = rinset[h - 1 - j];
        }
        gfx_span_a(x + in, y + j, w - 2 * in, c, a);
    }
}

void gui_draw_rrect(int x, int y, int w, int h, uint32_t color) { gfx_rfill_a(x, y, w, h, color, 256); }

void gui_draw_char_scaled(int x, int y, char ch, uint32_t color, int s) {
    const uint8_t *g = font8x18[(uint8_t)ch];
    for (int row = 0; row < 18; row++) {
        uint8_t bits = g[row];
        int col = 0;
        while (col < 8) {
            if (!(bits & (0x80 >> col))) { col++; continue; }
            int run = 0;
            while (col + run < 8 && (bits & (0x80 >> (col + run)))) run++;
            gfx_fill(x + col * s, y + row * s, run * s, s, color);
            col += run;
        }
    }
}

/* ---------------- Іконки меню (16x16, малюються x2) ---------------- */
static const char menu_icons[7][16][17] = {
    { /* APP */
        "                ",
        "                ",
        "  #####  +++++  ",
        "  #####  +++++  ",
        "  #####  +++++  ",
        "  #####  +++++  ",
        "  #####  +++++  ",
        "                ",
        "                ",
        "  +++++  #####  ",
        "  +++++  #####  ",
        "  +++++  #####  ",
        "  +++++  #####  ",
        "  +++++  #####  ",
        "                ",
        "                ",
    },
    { /* TERMINAL */
        "                ",
        "                ",
        " ############## ",
        " #............# ",
        " #............# ",
        " #            # ",
        " #            # ",
        " #  #         # ",
        " #   #        # ",
        " #    #       # ",
        " #   #        # ",
        " #  #   ++++  # ",
        " #            # ",
        " ############## ",
        "                ",
        "                ",
    },
    { /* EDITOR */
        "                ",
        "   ##########   ",
        "   #        #   ",
        "   #        #   ",
        "   # ++++++ #   ",
        "   #        #   ",
        "   # ++++++ #   ",
        "   #        #   ",
        "   # ++++++ #   ",
        "   #        #   ",
        "   # ++++   #   ",
        "   #        #   ",
        "   #        #   ",
        "   #        #   ",
        "   ##########   ",
        "                ",
    },
    { /* CALC */
        "                ",
        "  ############  ",
        "  #          #  ",
        "  # ++++++++ #  ",
        "  # ++++++++ #  ",
        "  # ++++++++ #  ",
        "  #          #  ",
        "  #          #  ",
        "  # ## ## ## #  ",
        "  #          #  ",
        "  # ## ## ## #  ",
        "  #          #  ",
        "  # ## ## ## #  ",
        "  #          #  ",
        "  ############  ",
        "                ",
    },
    { /* POWER */
        "                ",
        "       ++       ",
        "       ++       ",
        "       ++       ",
        "    ## ++ ##    ",
        "   ### ++ ###   ",
        "   ##  ++  ##   ",
        "  ##   ++   ##  ",
        "  ##   ++   ##  ",
        "  ##        ##  ",
        "  ##        ##  ",
        "   ##      ##   ",
        "   ###    ###   ",
        "    ########    ",
        "      ####      ",
        "                ",
    },
    { /* PAINT */
        "                ",
        "   ########     ",
        "  #        ##   ",
        " #  ++  ++   #  ",
        " # +        +#  ",
        " #   ##       # ",
        " #  ####   ++ # ",
        " # +####      # ",
        " #   ##    ## # ",
        "  #       ####  ",
        "  ##      ###   ",
        "   ########     ",
        "        ##      ",
        "         ##     ",
        "          #     ",
        "                ",
    },
    { /* FOLDER */
        "                ",
        "                ",
        " #####          ",
        " #+++++#######  ",
        " #+++++++++++#  ",
        " ############## ",
        " #            # ",
        " #  ++++++++  # ",
        " #            # ",
        " #  ++++++++  # ",
        " #            # ",
        " #  ++++      # ",
        " #            # ",
        " ############## ",
        "                ",
        "                ",
    },
};


/* нейтральна світло-сіра палітра іконок (читається на темно-сірих картках) */
static uint32_t menu_icon_color(char c) {
    switch (c) {
    case '#': return 0xF0F0F0;
    case '+': return 0xB4B4B4;
    case '.': return 0x8C8C8C;
    default:  return 0;
    }
}

static void draw_menu_icon(int id, int x, int y, int s) {
    if (id < 0 || id > 6) id = 0;
    for (int iy = 0; iy < 16; iy++)
        for (int ix = 0; ix < 16; ix++) {
            char c = menu_icons[id][iy][ix];
            if (c != ' ') gfx_fill(x + ix * s, y + iy * s, s, s, menu_icon_color(c));
        }
}

static void draw_task_icon(int x, int y, int icon_id, int scale)
{
    draw_menu_icon(icon_id, x, y, scale);
}

#define MENU_PW        320
#define MENU_PAD       10
#define MENU_SEARCH_H  34
#define MENU_ROW_H     50
#define MENU_CARD_H    44
#define MENU_ROWS      8      /* скільки рядків займає панель (по 50 px) */
#define MENU_SB_W      8
#define MENU_CARD_W    (MENU_PW - MENU_PAD - MENU_SB_W - 2 * MENU_PAD)

/* темно-сіра нейтральна тема */
#define MC_PANEL   0x2B2B2B
#define MC_BORDER  0x5A5A5A
#define MC_SEARCH  0x1C1C1C
#define MC_CARD    0x3A3A3A
#define MC_CARD_ON 0x5C5C5C
#define MC_TEXT    0xEDEDED
#define MC_DIM     0x9A9A9A
#define MC_THUMB   0xB0B0B0

typedef struct { const char *label; int icon; void (*fn)(void); } menu_item_t;
static menu_item_t menu_items[GUI_MENU_MAX];
static int  menu_count;
static char menu_q[24];
static int  menu_qlen;
static int  menu_vis[GUI_MENU_MAX];     /* відфільтровані індекси пунктів */
static int  menu_nvis;
static int  menu_sel = -1;              /* вибраний рядок у відфільтрованому списку */
static int  menu_scroll;

int gui_menu_add_ex(const char *label, int icon, void (*fn)(void)) {
    if (menu_count >= GUI_MENU_MAX) return -1;
    menu_items[menu_count].label = label;
    menu_items[menu_count].icon = icon;
    menu_items[menu_count].fn = fn;
    return menu_count++;
}

int gui_menu_add(const char *label, void (*fn)(void)) { return gui_menu_add_ex(label, GUI_ICON_APP, fn); }

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int menu_match(const char *label) {
    if (!menu_qlen) return 1;
    for (int i = 0; label[i]; i++) {
        int j = 0;
        while (j < menu_qlen && label[i + j] && lc(label[i + j]) == lc(menu_q[j])) j++;
        if (j == menu_qlen) return 1;
    }
    return 0;
}

static void menu_refilter(void) {
    menu_nvis = 0;
    for (int i = 0; i < menu_count; i++)
        if (menu_match(menu_items[i].label)) menu_vis[menu_nvis++] = i;
    menu_scroll = 0;
    menu_sel = menu_nvis ? 0 : -1;
}

/* висота панелі фіксована: MENU_ROWS рядків (порожні лишаються знизу), але не вище за екран */
static int menu_rows(void) {
    int fit = (H - GUI_TASKBAR_H - 4 - (3 * MENU_PAD + MENU_SEARCH_H) + (MENU_ROW_H - MENU_CARD_H)) / MENU_ROW_H;
    int r = MENU_ROWS < fit ? MENU_ROWS : fit;
    return r < 1 ? 1 : r;
}

static int menu_list_h(void) { return menu_rows() * MENU_ROW_H - (MENU_ROW_H - MENU_CARD_H); }

/* меню відкривається просто під верхньою панеллю (або над нижньою) */
static void menu_rect(int *x, int *y, int *w, int *h) {
    int ph = MENU_PAD + MENU_SEARCH_H + MENU_PAD + menu_list_h() + MENU_PAD;
    *x = START_X;
    *y = TASKBAR_TOP ? GUI_TASKBAR_H + 4 : H - GUI_TASKBAR_H - 4 - ph;
    *w = MENU_PW;
    *h = ph;
}

static void inv_menu(void) {
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    gui_invalidate_rect(x, y, w, h);
}

/* рядок відфільтрованого списку під курсором або -1 */
static int menu_item_at(int px, int py) {
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    int lx = x + MENU_PAD, ly = y + MENU_PAD + MENU_SEARCH_H + MENU_PAD;
    if (px < lx || px >= lx + MENU_CARD_W) return -1;
    int rel = py - ly;
    if (rel < 0) return -1;
    int row = rel / MENU_ROW_H;
    if (rel % MENU_ROW_H >= MENU_CARD_H || row >= menu_rows()) return -1;
    int v = menu_scroll + row;
    return v < menu_nvis ? v : -1;
}

static void menu_fit(void) {                 /* прокрутити так, щоб вибраний рядок було видно */
    int rows = menu_rows();
    if (menu_sel < menu_scroll) menu_scroll = menu_sel;
    if (menu_sel >= menu_scroll + rows) menu_scroll = menu_sel - rows + 1;
}

static void menu_close(void) {
    if (!start_open) return;
    inv_menu();
    start_open = 0;
    inv_taskbar();
}

static void menu_open(void) {
    if (!menu_count) { if (launcher) launcher(); return; }   /* меню порожнє — стара поведінка */
    start_open = 1;
    menu_qlen = 0;
    menu_refilter();
    inv_menu();
    inv_taskbar();
}

static void menu_toggle(void) { if (start_open) menu_close(); else menu_open(); }

static void menu_launch(int v) {
    if (v < 0 || v >= menu_nvis) return;
    void (*fn)(void) = menu_items[menu_vis[v]].fn;
    menu_close();
    if (fn) fn();
}

static void menu_key(const gui_key_t *k) {
    int c = k->code;
    if (c == GUI_KEY_ESC)   { menu_close(); return; }
    if (c == GUI_KEY_ENTER) { menu_launch(menu_sel); return; }
    if (c == GUI_KEY_UP || c == GUI_KEY_DOWN) {
        if (!menu_nvis) return;
        int n = menu_sel + (c == GUI_KEY_DOWN ? 1 : -1);
        if (n < 0) n = menu_nvis - 1;
        if (n >= menu_nvis) n = 0;
        menu_sel = n;
        menu_fit();
        inv_menu();
        return;
    }
    if (c == GUI_KEY_BACKSPACE) {
        if (menu_qlen) { menu_qlen--; menu_refilter(); inv_menu(); }
        return;
    }
    if (c >= 32 && c < 127 && menu_qlen < (int)sizeof(menu_q)) {
        menu_q[menu_qlen++] = (char)c;
        menu_refilter();
        inv_menu();
    }
}

/* колесо над меню: 1 — подію з'їдено */
static int menu_wheel_at(int px, int py, int delta) {
    if (!start_open) return 0;
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    if (!in_rect(px, py, x, y, w, h)) return 0;
    int rows = menu_rows();
    if (menu_nvis > rows) {
        int ns = menu_scroll - delta;
        if (ns < 0) ns = 0;
        if (ns > menu_nvis - rows) ns = menu_nvis - rows;
        if (ns != menu_scroll) { menu_scroll = ns; inv_menu(); }
    }
    return 1;
}

static void draw_menu(void) {
    if (!start_open || !menu_count) return;
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);

    /* напівпрозора панель із тонкою рамкою */
    gfx_rfill_a(x, y, w, h, MC_PANEL, 236);
    gfx_fill(x + 3, y, w - 6, 1, MC_BORDER);
    gfx_fill(x + 3, y + h - 1, w - 6, 1, MC_BORDER);
    gfx_fill(x, y + 3, 1, h - 6, MC_BORDER);
    gfx_fill(x + w - 1, y + 3, 1, h - 6, MC_BORDER);

    /* поле пошуку */
    int sx = x + MENU_PAD, sy = y + MENU_PAD, sw = w - 2 * MENU_PAD;
    int ty = sy + (MENU_SEARCH_H - 18) / 2;
    gfx_rfill_a(sx, sy, sw, MENU_SEARCH_H, MC_SEARCH, 256);
    if (menu_qlen) {
        char buf[sizeof(menu_q) + 1];
        for (int i = 0; i < menu_qlen; i++) buf[i] = menu_q[i];
        buf[menu_qlen] = 0;
        gfx_text(sx + 10, ty, buf, MC_TEXT);
        gfx_fill(sx + 10 + 8 * menu_qlen, ty, 2, 18, MC_TEXT);
    } else {
        gfx_fill(sx + 10, ty, 2, 18, MC_TEXT);
        gfx_text(sx + 20, ty, "Search", MC_DIM);
    }

    /* картки */
    int ly = y + MENU_PAD + MENU_SEARCH_H + MENU_PAD, rows = menu_rows();
    for (int r = 0; r < rows; r++) {
        int v = menu_scroll + r;
        if (v >= menu_nvis) break;
        const menu_item_t *it = &menu_items[menu_vis[v]];
        int cx = x + MENU_PAD, cy = ly + r * MENU_ROW_H;
        int on = (v == menu_sel);
        gfx_rfill_a(cx, cy, MENU_CARD_W, MENU_CARD_H, on ? MC_CARD_ON : MC_CARD, 256);
        draw_menu_icon(it->icon, cx + 8, cy + (MENU_CARD_H - 32) / 2, 2);
        gfx_text(cx + 8 + 32 + 12, cy + (MENU_CARD_H - 18) / 2, it->label, on ? 0xFFFFFF : MC_TEXT);
        gfx_char(cx + MENU_CARD_W - 20, cy + (MENU_CARD_H - 18) / 2, '>', on ? 0xFFFFFF : MC_DIM);
    }
    if (!menu_nvis) gfx_text(x + MENU_PAD + 12, ly + 12, "No results", MC_DIM);

    /* смуга прокрутки */
    int lh = menu_list_h();
    int bx = x + w - MENU_PAD - MENU_SB_W;
    int th = lh, tyy = ly;
    gfx_rfill_a(bx, ly, MENU_SB_W, lh, MC_SEARCH, 256);
    if (menu_nvis > rows) {
        th = lh * rows / menu_nvis;
        if (th < 16) th = 16;
        tyy = ly + (lh - th) * menu_scroll / (menu_nvis - rows);
    }
    gfx_rfill_a(bx, tyy, MENU_SB_W, th, MC_THUMB, 256);
}

static int ws_x0(void) { return (W - GUI_WORKSPACES * WS_CELL_W) / 2; }

static void draw_taskbar(void)
{
    int ty = taskbar_y();
    int by = ty + (GUI_TASKBAR_H - TB_BTN_H) / 2;

    gfx_fill(0, ty, W, GUI_TASKBAR_H, C_TASKBAR);

    /* роздільна лінія: по нижньому краю для верхньої панелі, по верхньому для нижньої */
    gfx_fill(0, TASKBAR_TOP ? ty + GUI_TASKBAR_H - 1 : ty, W, 1, C_TASKLINE);

    //кнопка пуск
    int sbw = start_btn_w(), sbh = start_btn_h(), sby = start_btn_y();
    
    /* без фону й фаски: лише іконка прямо на панелі */
    draw_start_icon
    (
        START_X + start_open,
        sby + start_open,
        sbw, 
        sbh
    );
    
    /* робочі столи: цифри 1..N по центру панелі; поточний — золотий, зайняті — підсвічені */
    {
        int wx0 = ws_x0();
        int cy = ty + (GUI_TASKBAR_H - 18) / 2;
        for (int i = 0; i < GUI_WORKSPACES; i++) {
            int x = wx0 + i * WS_CELL_W;
            int has = 0;
            for (int j = 0; j < GUI_MAX_WINDOWS; j++)
                if (pool[j].used && pool[j].ws == i) { has = 1; break; }
            uint32_t tc = C_TASKTEXT;
            if (i == cur_ws) {
                gfx_rfill_a(x + 2, ty + 7, WS_CELL_W - 4, GUI_TASKBAR_H - 14, C_BORDER, 256);
                tc = 0x101010;
            } else if (has) {
                gfx_rfill_a(x + 2, ty + 7, WS_CELL_W - 4, GUI_TASKBAR_H - 14, C_TASKLINE, 256);
            } else {
                tc = 0x6A7080;
            }
            gfx_char(x + (WS_CELL_W - 8) / 2, cy, (char)('1' + i), tc);
        }
    }

    int count = count_windows(), bw = task_btn_w(count), k = 0;

    for (int i = 0;i < GUI_MAX_WINDOWS; i++)
    {
        if(!pool[i].used || pool[i].ws != cur_ws) continue;
        int x = task_x0() + k * (bw + 4);      /* той самий крок, що й у taskbar_click */
        k++;
        
        int scale = 1;

        if (TB_BTN_H >= 32)
            scale = 2;

        int iw = 16 * scale;
        int ih = 16 * scale;

        int ix = x + (bw - iw) / 2;
        int iy = by + (TB_BTN_H - ih) / 2;

        draw_task_icon(ix, iy, pool[i].icon, scale);

    }
    // Вихід, годинник і дата 
    gfx_text(W - 8 * 5 - 6,
         ty + (GUI_TASKBAR_H - 18) / 2,
         clock_txt,
         C_TASKTEXT);
}

static int taskbar_click(int x, int y)
{
    int ty = taskbar_y();
    if (y < ty || y >= ty + GUI_TASKBAR_H) return 0;

    int by = ty + (GUI_TASKBAR_H - TB_BTN_H) / 2;
    if (in_rect(x, y, START_X, start_btn_y(), start_btn_w(), start_btn_h()))
    {
        menu_toggle();
        return 1;
    }
    if (in_rect(x, y, ws_x0(), ty, GUI_WORKSPACES * WS_CELL_W, GUI_TASKBAR_H))
    {
        gui_workspace_switch((x - ws_x0()) / WS_CELL_W);
        return 1;
    }
    if (in_rect(x, y, W - 6 - 56, by, 56, TB_BTN_H))
    {
        quit_flag = 1;
        return 1;
    }

    int count = count_windows(), bw = task_btn_w(count), k = 0;
    gui_window_t *act = active_window();
    for(int i = 0; i < GUI_MAX_WINDOWS; i++)
    {
        if(!pool[i].used || pool[i].ws != cur_ws) continue;
        int bx = task_x0() + k * (bw + 4);
        k++;
        if(in_rect(x, y, bx, by, bw, TB_BTN_H))
        {
            gui_window_t *w =  &pool[i];
                
            if(w->minimized)
            {
                gui_window_focus(w);
            }
            else if(w == act)
            {
                gui_window_minimize(w);
            }
            else
            {
                gui_window_focus(w);
            }
            return 1;
        }
    }
    return 1;
}

/* Напівпрозоре вікно: малюємо його як зазвичай, а потім змішуємо клієнтську область
 * із тим, що було під нею (знімок береться смугами, щоб не тримати зайвий буфер). */
#define TR_BAND 64
static uint32_t trbuf[GUI_MAX_W * TR_BAND];

static void draw_window_alpha(const gui_window_t *win, int active, rect_t r) {
    rect_t saved = clip;
    int cx = client_x(win), cy = client_y(win);
    int cw = win->w - 2 * BORDER, ch = win->h - 2 * BORDER - GUI_TITLE_H;

    int x0 = imax(r.x, win->x), x1 = imin(r.x + r.w, win->x + win->w);
    int y0 = imax(r.y, win->y), y1 = imin(r.y + r.h, win->y + win->h);
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t ua = (uint32_t)win->alpha + 1, ia = 256 - ua;
    int w = x1 - x0;

    for (int by = y0; by < y1; by += TR_BAND) {
        int bh = imin(TR_BAND, y1 - by);
        for (int j = 0; j < bh; j++)
            copy_row(&trbuf[j * w], &back[(by + j) * W + x0], w);

        clip.x = x0; clip.y = by; clip.w = w; clip.h = bh;
        draw_window(win, active);

        int bx0 = imax(x0, cx), bx1 = imin(x1, cx + cw);
        for (int j = 0; j < bh; j++) {
            int y = by + j;
            if (y < cy || y >= cy + ch) continue;
            for (int x = bx0; x < bx1; x++) {
                uint32_t *p = &back[y * W + x];
                uint32_t s = *p;
                if (s == C_BORDER || s == C_BORDER_OFF) continue;   /* золото в кутах не чіпаємо */
                uint32_t b = trbuf[j * w + (x - x0)];
                uint32_t rr_ = (((s >> 16) & 255) * ua + ((b >> 16) & 255) * ia) >> 8;
                uint32_t gg_ = (((s >> 8) & 255) * ua + ((b >> 8) & 255) * ia) >> 8;
                uint32_t bb_ = ((s & 255) * ua + (b & 255) * ia) >> 8;
                *p = (rr_ << 16) | (gg_ << 8) | bb_;
            }
        }
    }
    clip = saved;
}

static void draw_scene(rect_t r) {
    clip = r;
#ifdef GUI_STATS
    stat_render_px += (unsigned long)r.w * (unsigned long)r.h;
#endif

    /* робочий стіл */
    if (r.y < desk_y1() && r.y + r.h > desk_y0()) {
        if (have_wall) gfx_blit_wall();
        else           gfx_fill(0, desk_y0(), W, desk_h(), C_DESKTOP);
    }

    gui_window_t *act = active_window();
    for (int i = 0; i < norder; i++) {
        gui_window_t *w = order[i];
        int off = 0;
        if (ws_anim) {                           /* слайд: малюємо вікна обох столів, кожен зі своїм зсувом */
            if (w->minimized) continue;
            if (w->ws == cur_ws)       off = ws_dir * (W - ws_off);
            else if (w->ws == ws_from) off = -ws_dir * ws_off;
            else continue;
        } else if (!win_visible(w)) continue;

        w->x += off;
        rect_t wr = { w->x, w->y, w->w, w->h };
        if (rect_overlap(&wr, &r)) {
            if (w->alpha < 255) draw_window_alpha(w, w == act, r);
            else                draw_window(w, w == act);
        }
        w->x -= off;
        if (off) continue;

        if (w == drop_target) {                  /* куди впаде вікно, яке тягнуть */
            const int t = 3;
            const uint32_t hc = 0xFFE27A;
            gfx_fill(w->x, w->y, w->w, t, hc);
            gfx_fill(w->x, w->y + w->h - t, w->w, t, hc);
            gfx_fill(w->x, w->y, t, w->h, hc);
            gfx_fill(w->x + w->w - t, w->y, t, w->h, hc);
        }
    }

    /* панель завдань */
    {
        rect_t tb = { 0, taskbar_y(), W, GUI_TASKBAR_H };
        if (rect_overlap(&tb, &r)) draw_taskbar();
    }
    draw_menu();
}

static void flip(rect_t r) {
    uint32_t *fb = (uint32_t *)(uintptr_t)framebuffer_addr;
    uint32_t stride = framebuffer_pitch / 4;
#ifdef GUI_STATS
    stat_flip_px += (unsigned long)r.w * (unsigned long)r.h;
#endif
    for (int y = r.y; y < r.y + r.h; y++)
        copy_row(fb + stride * (uint32_t)y + (uint32_t)r.x, &back[y * W + r.x], r.w);
}

static void flush(void) {
    if (!nrr && !nfr) return;

    rect_t out[2 * MAX_RECTS];
    int n = 0;

    for (int i = 0; i < nrr; i++) {
        rect_t q = rr[i];
        if (!rect_clip_screen(&q)) continue;
        draw_scene(q);
        out[n++] = q;
    }
    nrr = 0;
    for (int i = 0; i < nfr; i++) {
        rect_t q = fr[i];
        if (rect_clip_screen(&q)) out[n++] = q;
    }
    nfr = 0;

    int hide = 0, cxp, cyp, cwp, chp;
    if (mouse_cursor_bounds(&cxp, &cyp, &cwp, &chp)) {
        rect_t c = { cxp, cyp, cwp, chp };
        for (int i = 0; i < n; i++) if (rect_overlap(&out[i], &c)) { hide = 1; break; }
    }
    if (hide) mouse_cursor_hide();
    for (int i = 0; i < n; i++) flip(out[i]);
    if (hide) mouse_cursor_show();
}

static void update_clock(void) {
    RTC_TIME t = rtc_get_time();
    char s[8];
    int h = t.hour % 100, m = t.minute % 100;
    s[0] = (char)('0' + h / 10); s[1] = (char)('0' + h % 10); s[2] = ':';
    s[3] = (char)('0' + m / 10); s[4] = (char)('0' + m % 10); s[5] = 0;
    int changed = 0;
    for (int i = 0; i < 6; i++) if (clock_txt[i] != s[i]) { clock_txt[i] = s[i]; changed = 1; }
    if (changed) gui_invalidate_rect(W - 6 - 56 - 10 - 8 * 5 - 2, taskbar_y(), 8 * 5 + 6, GUI_TASKBAR_H);
}

int gui_init(void) {
    if (framebuffer_addr == 0 || framebuffer_bpp != 32) return 0;
    if (framebuffer_width > GUI_MAX_W || framebuffer_height > GUI_MAX_H) return 0;
    W = (int)framebuffer_width;
    H = (int)framebuffer_height;

    for (int i = 0; i < GUI_MAX_WINDOWS; i++) pool[i].used = 0;
    norder = 0;
    ntiles = 0;
    drag_win = size_win = press_win = 0;
    drop_target = 0;
    cur_ws = 0;
    ws_anim = 0; ws_off = 0;
    fm_x = fm_y = -1;
    cap_win = 0;
    press_btn = 0;
    nrr = nfr = 0;
    have_wall = 0;
    quit_flag = 0;
    launcher = 0;
    start_open = 0;
    menu_count = 0;
    menu_nvis = 0;
    menu_qlen = 0;
    menu_sel = -1;
    menu_scroll = 0;
    k_shift = k_ctrl = k_alt = k_caps = k_e0 = 0;
    k_super = k_super_used = 0;
    ticks = 0;
    clock_txt[0] = 0;
    update_clock();
    gui_invalidate_all();
    return 1;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gui_set_wallpaper_bmp(const uint8_t *d, uint32_t size) {
    have_wall = 0;
    if (W == 0 || !d || size < 54 || d[0] != 'B' || d[1] != 'M') return 0;

    uint32_t off = rd32(d + 10);
    int32_t  bw  = (int32_t)rd32(d + 18);
    int32_t  bh  = (int32_t)rd32(d + 22);
    uint32_t bpp = (uint32_t)(d[28] | (d[29] << 8));
    uint32_t cmp = rd32(d + 30);
    if (bpp != 24 || cmp != 0 || bw <= 0 || bh == 0) return 0;

    int top_down = bh < 0;
    uint32_t sw = (uint32_t)bw, sh = (uint32_t)(top_down ? -bh : bh);
    if (sw > 16384 || sh > 16384) return 0;

    uint32_t row = (sw * 3 + 3) & ~3u;
    if (off >= size || (uint64_t)off + (uint64_t)row * sh > size) return 0;

    uint32_t cw, ch;
    if ((uint64_t)sw * (uint32_t)H > (uint64_t)sh * (uint32_t)W) { ch = sh; cw = sh * (uint32_t)W / (uint32_t)H; }
    else                                                         { cw = sw; ch = sw * (uint32_t)H / (uint32_t)W; }
    if (cw == 0) cw = 1;
    if (ch == 0) ch = 1;
    uint32_t ox = (sw - cw) / 2, oy = (sh - ch) / 2;

    for (int x = 0; x < W; x++) xmap[x] = (int)(ox + (uint32_t)x * cw / (uint32_t)W);

    for (int y = 0; y < H; y++) {
        uint32_t sy = oy + (uint32_t)y * ch / (uint32_t)H;
        const uint8_t *line = d + off + (uint32_t)(top_down ? sy : sh - 1 - sy) * row;
        uint32_t *dst = &wall[y * W];
        for (int x = 0; x < W; x++) {
            const uint8_t *px = line + xmap[x] * 3;
            dst[x] = ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | px[0];
        }
    }
    have_wall = 1;
    gui_invalidate_all();
    return 1;
}

gui_window_t *gui_window_create(int x, int y, int w, int h, const char *title) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (pool[i].used) continue;
        gui_window_t *win = &pool[i];
        if (w < MIN_W) w = MIN_W;
        if (h < MIN_H) h = MIN_H;
        if (y < desk_y0()) y = desk_y0();      /* не під панеллю завдань */
        win->used = 1;
        win->x = x; win->y = y; win->w = w; win->h = h;
        win->fx = x; win->fy = y; win->fw = w; win->fh = h;   /* геометрія на випадок плаваючого режиму */
        win->floating = 0;
        win->alpha = 255;
        win->anim = 0; win->anim_new = 0; win->pend_rs = 0; win->dragging = 0;
        win->ws = cur_ws;
        win->title = title ? title : "";
        win->icon = GUI_ICON_APP;
        win->minimized = win->maximized = win->resizable = 0;
        win->ops = 0; win->ctx = 0;
        win->nwidgets = 0;
        for (int k = 0; k < GUI_MAX_WIDGETS; k++) win->widgets[k].used = 0;

        gui_window_t *prev = active_window();
        if (prev) gui_window_invalidate(prev);
        order[norder++] = win;

        if (GUI_TILING) {
            /* нове вікно не повинно ховатися під розгорнутим */
            for (int k = 0; k < norder; k++)
                if (order[k] != win && win_tiled(order[k])) order[k]->maximized = 0;
            tile_add(win);
            win->anim_new = 1;                 /* поява: росте з центру слота, інші вікна плавно відсуваються */
            tile_layout();
            win->anim_new = 0;
        }

        gui_window_invalidate(win);
        inv_taskbar();
        return win;
    }
    return 0;
}

void gui_window_set_icon(gui_window_t *win, int icon)
{
    if (!win || !win->used)
        return;

    if (icon < 0 || icon >= 7)
        icon = GUI_ICON_APP;

    win->icon = icon;
    if (icon == GUI_ICON_TERMINAL && GUI_TERMINAL_ALPHA < 255)
        gui_window_set_alpha(win, GUI_TERMINAL_ALPHA);
    inv_taskbar();
}

void gui_window_set_alpha(gui_window_t *win, int alpha)
{
    if (!win || !win->used) return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    win->alpha = alpha;
    gui_window_invalidate(win);
}

void gui_window_close(gui_window_t *win) {
    if (!win || !win->used) return;
    gui_window_invalidate(win);
    reset_interaction(win);

    const gui_ops_t *ops = win->ops;
    void *ctx = win->ctx;

    int i = 0;
    while (i < norder && order[i] != win) i++;
    for (; i < norder - 1; i++) order[i] = order[i + 1];
    if (norder > 0) norder--;
    win->used = 0;
    win->anim = 0; win->pend_rs = 0;

    tile_remove(win);
    tile_layout_ws(win->ws);                 /* решта вікон плавно займає звільнене місце */

    if (ops && ops->close) ops->close(win, ctx);

    gui_window_t *a = active_window();
    if (a) gui_window_invalidate(a);
    inv_taskbar();
}

void gui_window_request_close(gui_window_t *win) {
    if (!win || !win->used) return;
    if (win->ops && win->ops->can_close && !win->ops->can_close(win, win->ctx)) return;
    gui_window_close(win);
}

void gui_window_invalidate_title(gui_window_t *win) {
    if (!win || !win->used) return;
    gui_invalidate_rect(win->x, win->y, win->w, GUI_TITLE_H + 2 * BORDER);
    inv_taskbar();
}

static uint32_t tmprow[GUI_MAX_W];
static void move_block(rect_t s, int dx, int dy) {
    if (dy > 0) {
        for (int y = s.y + s.h - 1; y >= s.y; y--) {
            copy_row(tmprow, &back[y * W + s.x], s.w);
            copy_row(&back[(y + dy) * W + s.x + dx], tmprow, s.w);
        }
    } else {
        for (int y = s.y; y < s.y + s.h; y++) {
            copy_row(tmprow, &back[y * W + s.x], s.w);
            copy_row(&back[(y + dy) * W + s.x + dx], tmprow, s.w);
        }
    }
}

void gui_window_move(gui_window_t *win, int x, int y) {
    if (!win || !win->used || win->maximized) return;
    if (win_tiled(win)) return;              /* позицію тайлового вікна задає тайлінг */
    int y0 = desk_y0(), y1 = desk_y1();
    int min_x = 40 - win->w, max_x = W - 40;
    int min_y = y0,          max_y = y1 - GUI_TITLE_H;
    if (x < min_x) x = min_x;
    if (x > max_x) x = max_x;
    if (y < min_y) y = min_y;
    if (y > max_y) y = max_y;
    if (x == win->x && y == win->y) return;

    int dx = x - win->x, dy = y - win->y;

    /* швидкий шлях (копіювання блоку) — лише коли вікно ціле лежить на робочому столі до і після */
    if (!win->minimized && win->alpha >= 255 && win == active_window() &&
        win->y >= y0 && y >= y0 &&
        win->y + win->h <= y1 && y + win->h <= y1) {
        flush();

        rect_t oldr = { win->x, win->y, win->w, win->h };
        rect_t newr = { x, y, win->w, win->h };
        rect_t os = oldr;                      
        rect_t ns = newr;               
        rect_t ds = oldr;                    

        if (rect_clip_screen(&os) && rect_clip_screen(&ns)) {
            ds.x = os.x + dx; ds.y = os.y + dy; ds.w = os.w; ds.h = os.h;
            if (rect_clip_screen(&ds)) {
                rect_t src = { ds.x - dx, ds.y - dy, ds.w, ds.h };
                move_block(src, dx, dy);
                win->x = x; win->y = y;
                add_render_minus(os, newr);
                add_render_minus(ns, ds);              
                list_add(fr, &nfr, ns);

                /* прозорі кути заокруглення скопіювались зі старим фоном — перемалювати їх */
                int r = WIN_R;
                gui_invalidate_rect(x,              y,              r, r);
                gui_invalidate_rect(x + win->w - r, y,              r, r);
                gui_invalidate_rect(x,              y + win->h - r, r, r);
                gui_invalidate_rect(x + win->w - r, y + win->h - r, r, r);
                return;
            }
        }
    }

    gui_window_invalidate(win);
    win->x = x; win->y = y;
    gui_window_invalidate(win);
}

static widget_t *add_widget(gui_window_t *win, int type) {
    if (!win || win->nwidgets >= GUI_MAX_WIDGETS) return 0;
    widget_t *b = &win->widgets[win->nwidgets++];
    b->used = (uint8_t)type;
    b->fn = 0; b->ctx = 0; b->color = 0;
    return b;
}

int gui_add_label(gui_window_t *win, int x, int y, const char *text, uint32_t color) {
    widget_t *b = add_widget(win, W_LABEL);
    if (!b) return -1;
    b->x = x; b->y = y; b->w = 8 * gui_strlen(text); b->h = 18;
    b->text = text; b->color = color;
    gui_window_invalidate_client(win);
    return win->nwidgets - 1;
}

int gui_add_button(gui_window_t *win, int x, int y, int w, int h, const char *text,
                   gui_click_fn fn, void *ctx) {
    widget_t *b = add_widget(win, W_BUTTON);
    if (!b) return -1;
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->text = text; b->fn = fn; b->ctx = ctx;
    gui_window_invalidate_client(win);
    return win->nwidgets - 1;
}

static int kbd_poll(gui_key_t *out) {
    uint8_t s = inb(0x64);
    if (!(s & 0x01)) return -1;
    if (s & 0x20) { ps2_mouse_feed(inb(0x60)); return 0; }   /* байт PS/2-миші: віддаємо їй */
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { k_e0 = 1; return 0; }

    int rel = (sc & 0x80) != 0;
    sc &= 0x7F;
    int ext = k_e0;
    k_e0 = 0;

    /* Super (Win): E0 5B — лівий, E0 5C — правий. Меню відкриваємо при відпусканні,
     * але лише якщо між натисканням і відпусканням не було інших клавіш (щоб Super+щось не відкривало меню) */
    if (ext && (sc == 0x5B || sc == 0x5C)) {
        if (!rel) {
            if (!k_super) { k_super = 1; k_super_used = 0; }
            return 0;
        }
        int fire = k_super && !k_super_used;
        k_super = 0;
        if (!fire) return 0;
        out->code = GUI_KEY_SUPER;
        out->ctrl = out->alt = out->shift = 0;
        return 1;
    }

    if (sc == 0x2A || sc == 0x36) { k_shift = !rel; return 0; }
    if (sc == 0x1D)               { k_ctrl  = !rel; return 0; }
    if (sc == 0x38)               { k_alt   = !rel; return 0; }
    if (rel) return 0;
    if (k_super) k_super_used = 1;               /* Super використано як модифікатор */
    if (sc == 0x3A)               { k_caps = !k_caps; return 0; }

    int code = 0;
    if (ext) {
        switch (sc) {
        case 0x48: code = GUI_KEY_UP;     break;
        case 0x50: code = GUI_KEY_DOWN;   break;
        case 0x4B: code = GUI_KEY_LEFT;   break;
        case 0x4D: code = GUI_KEY_RIGHT;  break;
        case 0x47: code = GUI_KEY_HOME;   break;
        case 0x4F: code = GUI_KEY_END;    break;
        case 0x53: code = GUI_KEY_DELETE; break;
        case 0x49: code = GUI_KEY_PGUP;   break;
        case 0x51: code = GUI_KEY_PGDN;   break;
        default: break;
        }
    }
    if (!code) {
        switch (sc) {
        case 0x0E: code = GUI_KEY_BACKSPACE; break;
        case 0x1C: code = GUI_KEY_ENTER;     break;
        case 0x0F: code = GUI_KEY_TAB;       break;
        case 0x01: code = GUI_KEY_ESC;       break;
        case 0x3E: code = GUI_KEY_F4;        break;
        default:
            if (sc < 128) {
                char c = k_shift ? keyboard_shift_map[sc]
                       : k_caps  ? keyboard_caps_map[sc]
                                 : keyboard_map[sc];
                code = (unsigned char)c;
            }
            break;
        }
    }
    if (!code) return 0;

    out->code = code;
    out->ctrl = (uint8_t)k_ctrl;
    out->alt = (uint8_t)k_alt;
    out->shift = (uint8_t)k_shift;
    return 1;
}

static void cycle_windows(void) {
    for (int i = 0; i < norder; i++)         /* найнижче видиме вікно на поточному столі йде нагору */
        if (win_visible(order[i])) { gui_window_focus(order[i]); return; }
}

/* клавіша 1..9 (разом із Shift дає !@#$%^&*( ) -> номер столу або -1 */
static int ws_key(int code) {
    static const char sh[] = "!@#$%^&*(";
    if (code >= '1' && code <= '9') return code - '1';
    for (int i = 0; i < 9; i++) if (code == sh[i]) return i;
    return -1;
}

/* найближче видиме вікно в напрямку (dx, dy) від активного */
static gui_window_t *find_dir(gui_window_t *a, int dx, int dy) {
    int ax = a->x + a->w / 2, ay = a->y + a->h / 2;
    gui_window_t *best = 0;
    int bestd = 0x7FFFFFFF;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_window_t *w = &pool[i];
        if (!win_visible(w) || w == a) continue;
        int ddx = (w->x + w->w / 2) - ax, ddy = (w->y + w->h / 2) - ay;
        if (dx && ddx * dx <= 0) continue;
        if (dy && ddy * dy <= 0) continue;
        int along = dx ? iabs(ddx) : iabs(ddy);
        int perp  = dx ? iabs(ddy) : iabs(ddx);
        int d = along + 2 * perp;
        if (d < bestd) { bestd = d; best = w; }
    }
    return best;
}

static void handle_key(const gui_key_t *k) {
    gui_window_t *act = active_window();

    if (k->code == GUI_KEY_SUPER) { menu_toggle(); return; }          /* клавіша Super — меню «Пуск» */
    if (k->ctrl && !k->alt && k->code == GUI_KEY_ESC) { menu_toggle(); return; }
    if (start_open && !k->ctrl && !k->alt) { menu_key(k); return; }   /* пошук і навігація в меню */

    /* ---- керування вікнами (кнопок у заголовку немає, тож усе з клавіатури) ---- */
    if (k->alt && k->code == GUI_KEY_F4) { if (act) gui_window_request_close(act); return; }                       /* закрити */
    if (k->ctrl && !k->alt && (k->code == 'w' || k->code == 'W')) { if (act) gui_window_request_close(act); return; } /* закрити */
    if (k->alt && !k->ctrl && (k->code == 'm' || k->code == 'M')) { if (act) gui_window_minimize(act); return; }       /* згорнути */
    if (k->alt && !k->ctrl && k->code == GUI_KEY_ENTER)           { if (act) gui_window_toggle_maximize(act); return; } /* розгорнути */
    if (k->alt && !k->ctrl && (k->code == 'f' || k->code == 'F')) { if (act) gui_window_toggle_floating(act); return; } /* плаваюче/тайл */
    if (k->alt && k->code == GUI_KEY_TAB) { cycle_windows(); return; }

    /* робочі столи: Alt+N — перейти, Alt+Shift+N — відправити активне вікно на стіл N */
    if (k->alt && !k->ctrl) {
        int d = ws_key(k->code);
        if (d >= 0 && d < GUI_WORKSPACES) {
            if (k->shift) { if (act) gui_window_move_to_workspace(act, d); }
            else          gui_workspace_switch(d);
            return;
        }
    }

    /* Ctrl+Alt+←/→ — сусідній робочий стіл (те саме, що жест трьома пальцями) */
    if (k->ctrl && k->alt && (k->code == GUI_KEY_LEFT || k->code == GUI_KEY_RIGHT)) {
        gui_workspace_step(k->code == GUI_KEY_RIGHT ? 1 : -1);
        return;
    }

    /* Alt+стрілки — фокус на сусіднє вікно; Alt+Shift+стрілки — помінятися з ним місцями */
    if (k->alt && !k->ctrl && act &&
        (k->code == GUI_KEY_LEFT || k->code == GUI_KEY_RIGHT || k->code == GUI_KEY_UP || k->code == GUI_KEY_DOWN)) {
        int dx = (k->code == GUI_KEY_LEFT) ? -1 : (k->code == GUI_KEY_RIGHT) ? 1 : 0;
        int dy = (k->code == GUI_KEY_UP)   ? -1 : (k->code == GUI_KEY_DOWN)  ? 1 : 0;
        gui_window_t *t = find_dir(act, dx, dy);
        if (t) {
            if (k->shift && win_tiled(act) && win_tiled(t)) tile_swap(act, t);
            else if (!k->shift) gui_window_focus(t);
        }
        return;
    }

    if (k->ctrl && k->alt && (k->code == 't' || k->code == 'T')) { if (launcher) launcher(); return; }

    if (act && act->ops && act->ops->key) { act->ops->key(act, k, act->ctx); return; }
    if (k->code == GUI_KEY_ESC) quit_flag = 1;
}

static gui_window_t *window_at(int x, int y) {
    for (int i = norder - 1; i >= 0; i--) {
        if (!win_visible(order[i])) continue;
        if (in_rect(x, y, order[i]->x, order[i]->y, order[i]->w, order[i]->h)) return order[i];
    }
    return 0;
}

static widget_t *button_at(gui_window_t *win, int x, int y) {
    int lx = x - client_x(win), ly = y - client_y(win);
    for (int i = win->nwidgets - 1; i >= 0; i--) {
        widget_t *b = &win->widgets[i];
        if (b->used == W_BUTTON && in_rect(lx, ly, b->x, b->y, b->w, b->h)) return b;
    }
    return 0;
}

static void invalidate_widget(gui_window_t *win, const widget_t *b) {
    gui_invalidate_rect(client_x(win) + b->x, client_y(win) + b->y, b->w, b->h);
}

static void on_press(int x, int y) {
    if (ws_anim) return;                     /* під час слайду кліки ігноруємо */
    if (start_open) {
        int mx, my, mw, mh;
        menu_rect(&mx, &my, &mw, &mh);
        if (in_rect(x, y, mx, my, mw, mh)) {
            menu_launch(menu_item_at(x, y));
            return;
        }
        /* клік по самій кнопці «Пуск» закриє меню в taskbar_click, решта — закриває тут */
        if (!in_rect(x, y, START_X, start_btn_y(), start_btn_w(), start_btn_h()))
            menu_close();
    }

    if (taskbar_click(x, y)) return;

    gui_window_t *win = window_at(x, y);
    if (!win) return;
    gui_window_focus(win);

    /* заголовок: перетягування. Плаваюче вікно рухається одразу; тайлове, якщо потягнути
     * за заголовок далі за поріг, само стає плаваючим і йде за курсором (див. gui_frame). */
    if (y < win->y + BORDER + GUI_TITLE_H) {
        if (!win->maximized) {
            drag_win = win;
            drag_dx = x - win->x;
            drag_dy = y - win->y;
            drag_px = x;
            drag_py = y;
        }
        return;
    }

    if (win->resizable && !win->maximized && !win_tiled(win) &&
        in_rect(x, y, win->x + win->w - GRIP, win->y + win->h - GRIP, GRIP, GRIP)) {
        size_win = win;
        size_mx = x; size_my = y;
        size_w = win->w; size_h = win->h;
        return;
    }

    widget_t *b = button_at(win, x, y);
    if (b) {
        press_win = win;
        press_btn = b;
        invalidate_widget(win, b);
        return;
    }

    if (win->ops && win->ops->click)
        win->ops->click(win, x - client_x(win), y - client_y(win), win->ctx);

    if (win->used && win->ops && (win->ops->drag || win->ops->release)) {
        cap_win = win;
        cap_lx = x; cap_ly = y;
    }
}

static void on_release(int x, int y) {
    gui_window_t *dw = drag_win;
    drag_win = 0;
    size_win = 0;

    /* тайлове вікно відпустили: над іншим тайлом — міняємо місцями, інакше воно повертається у свій слот */
    if (dw && dw->used && dw->dragging) {
        gui_window_t *t = tile_at(x, y, dw);
        dw->dragging = 0;
        if (drop_target) { gui_window_invalidate(drop_target); drop_target = 0; }
        if (t) tile_swap(dw, t);
        else   tile_layout();
    }

    if (cap_win) {
        gui_window_t *cw = cap_win;
        cap_win = 0;
        if (cw->used && cw->ops && cw->ops->release)
            cw->ops->release(cw, x - client_x(cw), y - client_y(cw), cw->ctx);
    }

    if (!press_btn) return;
    gui_window_t *win = press_win;
    widget_t *b = press_btn;
    press_win = 0;
    press_btn = 0;

    invalidate_widget(win, b);
    if (win->used && button_at(win, x, y) == b && window_at(x, y) == win && b->fn) {
        b->fn(b->ctx);
        gui_window_invalidate_client(win);          /* колбек міг змінити підписи */
    }
}

void gui_frame(void) {
    gui_key_t k;
    for (int n = 0; n < 64; n++) {
        int r = kbd_poll(&k);
        if (r < 0) break;
        if (r > 0) handle_key(&k);
    }

    if (mouse_poll()) {
        const mouse_state_t *m = mouse_get();

        if (start_open) {
            int hv = menu_item_at(m->x, m->y);
            if (hv >= 0 && hv != menu_sel) { menu_sel = hv; inv_menu(); }
        }

#if GUI_FOCUS_FOLLOWS_MOUSE
        {
            int moved = (m->x != fm_x || m->y != fm_y);
            fm_x = m->x; fm_y = m->y;
            /* лише коли мишу справді посунули (тож Alt+стрілки/Alt+Tab не збиваються), без натиснутих кнопок
             * і поза перетягуванням, зміною розміру та меню «Пуск» */
            if (moved && !m->buttons && !start_open && !drag_win && !size_win && !cap_win) {
                gui_window_t *hw = window_at(m->x, m->y);
                if (hw && hw != active_window()) gui_window_focus(hw);
            }
        }
#endif

        if (m->pressed & MOUSE_BTN_LEFT) on_press(m->x, m->y);

        if (drag_win && drag_win->used && (m->buttons & MOUSE_BTN_LEFT)) {
            gui_window_t *dw = drag_win;
            if (win_tiled(dw)) {
                if (!dw->dragging && iabs(m->x - drag_px) + iabs(m->y - drag_py) >= GUI_DRAG_FLOAT_PX) {
                    if (k_shift) {
                        /* Shift + тяг: вікно відривається від сітки й стає плаваючим */
                        dw->fx = dw->x; dw->fy = dw->y; dw->fw = dw->w; dw->fh = dw->h;
                        gui_window_set_floating(dw, 1);
                        drag_dx = m->x - dw->x;
                        drag_dy = m->y - dw->y;
                    } else {
                        /* звичайний тяг: вікно йде за курсором, на відпусканні міняється місцями з тайлом під ним */
                        dw->dragging = 1;
                        dw->anim = 0; dw->pend_rs = 0;
                        drag_dx = m->x - dw->x;
                        drag_dy = m->y - dw->y;
                    }
                }
                if (dw->dragging) tile_drag_to(dw, m->x - drag_dx, m->y - drag_dy, m->x, m->y);
            }
            if (!win_tiled(dw)) gui_window_move(dw, m->x - drag_dx, m->y - drag_dy);
        }

        if (size_win && (m->buttons & MOUSE_BTN_LEFT))
            gui_window_resize(size_win, size_w + (m->x - size_mx), size_h + (m->y - size_my));

        if (cap_win && (m->buttons & MOUSE_BTN_LEFT) &&
            (m->x != cap_lx || m->y != cap_ly)) {
            cap_lx = m->x; cap_ly = m->y;
            if (cap_win->used && cap_win->ops && cap_win->ops->drag)
                cap_win->ops->drag(cap_win, m->x - client_x(cap_win),
                                   m->y - client_y(cap_win), cap_win->ctx);
        }

        if (m->wheel && !menu_wheel_at(m->x, m->y, m->wheel)) {
            gui_window_t *w = window_at(m->x, m->y);
            if (w && w->ops && w->ops->wheel) w->ops->wheel(w, m->wheel, w->ctx);
        }

        if (m->released & MOUSE_BTN_LEFT) on_release(m->x, m->y);
    }

    {   /* жест трьома пальцями: пальці ліворуч — наступний стіл, праворуч — попередній */
        int sw = tp_swipe_pop();
        if (sw) gui_workspace_step(GUI_WS_SWIPE_INVERT ? sw : -sw);
    }

    if ((++ticks & 0xFFFF) == 0) update_clock();
    anim_step();
    flush();
}

void gui_run(void) {
    quit_flag = 0;
    flush();
    while (!quit_flag) gui_frame();
    mouse_cursor_hide();
}