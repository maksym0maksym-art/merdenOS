#include <stdint.h>
#include "gui.h"
#include "../shell/shell.h"
#include "../shell/graphic/console.h"
#include "../../kernel/headers/usbfs.h"
#include "../../standarts_lib/charset.h"

#define TERM_MAX       3
#define TERM_COLS_MAX  240
#define TERM_LINES     300
#define INPUT_MAX      200
#define HIST_N         16

#define C_BG      0x101418
#define C_FG      0xE8E8E8
#define C_PROMPT  0x5AA9FF
#define C_CURSOR  0xE8E8E8

#define MK(ch, color) (((uint32_t)(uint8_t)(ch) << 24) | ((uint32_t)(color) & 0xFFFFFF))

/* ---------------------------------------------------------------- */
/*  Спеціальні "символи" комірки, які малюються вручну (не шрифтом)  */
/*  Коди 1..5 не конфліктують з \t (9), \n (10), \r (13)             */
/* ---------------------------------------------------------------- */
#define SP_ROSE_L  1     /* ліва половина троянди (8x18)  */
#define SP_ROSE_R  2     /* права половина троянди (8x18) */
#define SP_BOX_TL  3     /* ┌ */
#define SP_BOX_H   4     /* ─ */
#define SP_BOX_BL  5     /* └ */

#define CELL_W 8
#define CELL_H 18

/* Друга рядок промпта:  └─>> <ввід>   — 5 клітинок */
#define PFX_LEN 5
static const char prompt_prefix[PFX_LEN] = { SP_BOX_BL, SP_BOX_H, '>', '>', ' ' };

typedef struct {
    int used;
    gui_window_t *win;

    uint32_t cells[TERM_LINES][TERM_COLS_MAX];
    int cur;                         /* абсолютний номер поточного рядка */
    int col;                         /* позиція виводу в ньому */
    int cols, rows;
    int scroll;                      /* на скільки рядків прогорнули назад (0 = внизу) */

    char line[INPUT_MAX + 1];
    int  len, pos;

    char hist[HIST_N][INPUT_MAX + 1];
    int  hcount, hhead;              /* hhead — куди писати наступний запис */
    int  hpos;                       /* -1 — не гортаємо історію */
    char saved[INPUT_MAX + 1];       /* що було набрано до гортання історії */
} term_t;

static term_t terms[TERM_MAX];
static term_t *sink_term;

/* ---------------------------------------------------------------- */
/*  Невеликі рядкові функції (щоб не залежати від stdio.h)           */
/* ---------------------------------------------------------------- */
static int t_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "usb0:/dir" без кінцевого пробілу, а без USB-диска — "/" */
static void t_dir(char *out, int n) {
    const char *d = usbfs_prompt();          /* "usb0:/dir " з пробілом в кінці, або "" */
    int len = 0;
    while (d[len] && len < n - 1) { out[len] = d[len]; len++; }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = 0;
    if (len == 0 && n > 1) { out[0] = '/'; out[1] = 0; }
}

/* ---------------------------------------------------------------- */
/*  Троянда 16x18 (дві клітинки). '.' — прозорий піксель             */
/* ---------------------------------------------------------------- */
static const char *const rose_rows[18] = {
    "................",
    "......llll......",
    "....llrrrrll....",
    "...lrrddddrrrl..",
    "..lrdddlllddrrl.",
    "..lrdlrrrrldrrl.",
    ".lrrdlrddrlddrrl",
    ".lrrrdlrrldrrrrl",
    ".lrrrrdlllddrrrl",
    "..lrrrrddddrrrl.",
    "...lrrrrrrrrrl..",
    ".....ddrrrdd....",
    "..ggg..GG..ggg..",
    "...gggg.GG.gggg.",
    "......ggGGgg....",
    ".......GG.......",
    ".......GG.......",
    ".......GG.......",
};

static uint32_t rose_color(char ch) {
    switch (ch) {
        case 'd': return 0x8B0A14;   /* тінь */
        case 'r': return 0xC8102E;   /* пелюстки */
        case 'l': return 0xFF4A4A;   /* світла кромка */
        case 'g': return 0x5FA02A;   /* листя */
        case 'G': return 0x2E6A12;   /* стебло */
    }
    return 0;
}

/* half: 0 — ліва, 1 — права. Однакові сусідні пікселі зливаємо в один rect */
static void draw_rose_half(int x, int y, int half) {
    for (int r = 0; r < CELL_H; r++) {
        const char *row = rose_rows[r] + half * 8;
        int c = 0;
        while (c < 8) {
            char ch = row[c];
            if (ch == '.') { c++; continue; }
            int e = c + 1;
            while (e < 8 && row[e] == ch) e++;
            gui_draw_rect(x + c, y + r, e - c, 1, rose_color(ch));
            c = e;
        }
    }
}

/* Малює одну клітинку: звичайний символ або спеціальний (рамка / троянда) */
static void draw_cell(int x, int y, char ch, uint32_t color) {
    switch (ch) {
        case ' ':
            return;
        case SP_ROSE_L:
            draw_rose_half(x, y, 0);
            return;
        case SP_ROSE_R:
            draw_rose_half(x, y, 1);
            return;
        case SP_BOX_H:                                   /* ─ */
            gui_draw_rect(x, y + 8, CELL_W, 2, color);
            return;
        case SP_BOX_TL:                                  /* ┌ */
            gui_draw_rect(x + 3, y + 8, 5, 2, color);
            gui_draw_rect(x + 3, y + 8, 2, CELL_H - 8, color);
            return;
        case SP_BOX_BL:                                  /* └ */
            gui_draw_rect(x + 3, y, 2, 10, color);
            gui_draw_rect(x + 3, y + 8, 5, 2, color);
            return;
        default:
            gui_draw_char(x, y, ch, color);
    }
}

/* ---------------------------------------------------------------- */
/*  Буфер рядків                                                     */
/* ---------------------------------------------------------------- */
static uint32_t *line_ptr(term_t *t, int abs) { return t->cells[abs % TERM_LINES]; }

static void clear_line(term_t *t, int abs) {
    uint32_t *l = line_ptr(t, abs);
    for (int c = 0; c < TERM_COLS_MAX; c++) l[c] = MK(' ', C_FG);
}

static void term_clear(term_t *t) {
    for (int i = 0; i < TERM_LINES; i++) clear_line(t, i);
    t->cur = 0;
    t->col = 0;
    t->scroll = 0;
}

static void term_newline(term_t *t) {
    t->cur++;
    t->col = 0;
    clear_line(t, t->cur);
}

static void term_put(term_t *t, char c, uint32_t color) {
    if (c == '\r') return;
    if (c == '\n') { term_newline(t); return; }
    if (c == '\t') {
        int n = 4 - (t->col % 4);
        while (n--) term_put(t, ' ', color);
        return;
    }
    if (!cs_printable((unsigned char)c) && !(c >= SP_ROSE_L && c <= SP_BOX_BL)) c = '?';

    int cols = t->cols < 1 ? 1 : (t->cols > TERM_COLS_MAX ? TERM_COLS_MAX : t->cols);
    if (t->col >= cols) term_newline(t);
    line_ptr(t, t->cur)[t->col++] = MK(c, color);
}

static void term_puts(term_t *t, const char *s, uint32_t color) {
    while (*s) term_put(t, *s++, color);
}

/* Перший рядок промпта (в історії):  ┌──{rose🌹rose}-{usb1:/} */
static void term_header(term_t *t) {
    char dir[64];
    t_dir(dir, sizeof dir);

    term_put(t, SP_BOX_TL, C_PROMPT);
    term_put(t, SP_BOX_H,  C_PROMPT);
    term_put(t, SP_BOX_H,  C_PROMPT);
    term_puts(t, "{rose", C_PROMPT);
    term_put(t, SP_ROSE_L, C_PROMPT);
    term_put(t, SP_ROSE_R, C_PROMPT);
    term_puts(t, "rose}-{", C_PROMPT);
    term_puts(t, dir, C_FG);
    term_puts(t, "}", C_PROMPT);
    term_newline(t);
}

/* Другий рядок промпта:  └─>>  (для відлуння введеної команди в історії) */
static void term_prefix(term_t *t) {
    for (int i = 0; i < PFX_LEN; i++) term_put(t, prompt_prefix[i], C_PROMPT);
}

/* Сюди шел "друкує": console_puts_color() / console_putc() перенаправлені в це */
static void sink_fn(char c, uint32_t color) {
    if (sink_term) term_put(sink_term, c, color ? color : C_FG);
}

/* ---------------------------------------------------------------- */
/*  Перемальовування                                                 */
/* ---------------------------------------------------------------- */
static void inv_client(term_t *t) { gui_window_invalidate_client(t->win); }

static void inv_input_row(term_t *t) {
    if (t->scroll != 0) return;                     /* рядок вводу не видно */
    int x, y, cw, ch;
    gui_window_client(t->win, &x, &y, &cw, &ch);
    int rows = ch / CELL_H;
    if (rows < 1) rows = 1;
    int top = t->cur - (rows - 1);                  /* поки рядків мало, вивід іде від верху вікна */
    if (top < 0) top = 0;
    gui_invalidate_rect(x, y + (t->cur - top) * CELL_H, cw, CELL_H);
}

static void draw_input(term_t *t, int x, int y, int cols) {
    int plen = PFX_LEN;
    int total = plen + t->len;
    int ci = plen + t->pos;
    int start = ci + 1 > cols ? ci + 1 - cols : 0;

    for (int i = start; i < total && i < start + cols; i++) {
        char ch = i < plen ? prompt_prefix[i] : t->line[i - plen];
        draw_cell(x + (i - start) * CELL_W, y, ch, i < plen ? C_PROMPT : C_FG);
    }

    int cx = x + (ci - start) * CELL_W;
    gui_draw_rect(cx, y, CELL_W, CELL_H, C_CURSOR);
    if (ci < total) {
        char ch = ci < plen ? prompt_prefix[ci] : t->line[ci - plen];
        draw_cell(cx, y, ch, C_BG);
    }
}

static void term_paint(gui_window_t *w, int x, int y, int cw, int ch, void *ctx) {
    (void)w;
    term_t *t = (term_t *)ctx;

    int cols = cw / CELL_W;
    if (cols > TERM_COLS_MAX) cols = TERM_COLS_MAX;
    if (cols < 1) cols = 1;
    int rows = ch / CELL_H;
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;

    int cx, cy, cwid, chei;
    gui_get_clip(&cx, &cy, &cwid, &chei);
    gui_draw_rect(x, y, cw, ch, C_BG);

    int bottom = t->cur - t->scroll;
    int top = bottom - (rows - 1);
    if (top < 0) top = 0;
    int oldest = t->cur - TERM_LINES + 1;
    if (oldest < 0) oldest = 0;

    for (int r = 0; r < rows; r++) {
        int ry = y + r * CELL_H;
        if (ry + CELL_H <= cy || ry >= cy + chei) continue;      /* поза ділянкою, що перемальовується */

        int abs = top + r;
        if (abs == t->cur) { draw_input(t, x, ry, cols); continue; }
        if (abs < oldest || abs > t->cur) continue;

        const uint32_t *ln = line_ptr(t, abs);
        for (int c = 0; c < cols; c++) {
            char chr = (char)(ln[c] >> 24);
            draw_cell(x + c * CELL_W, ry, chr, ln[c] & 0xFFFFFF);
        }
    }
}

/* ---------------------------------------------------------------- */
/*  Команди                                                          */
/* ---------------------------------------------------------------- */
/* Ці команди малюють на весь екран або чекають клавіші самі — у вікні вони зламали б картинку */
static const char *const blocked[] = { "gui", "mouse", "doge", "IDE", "IDE_debug", "interrput", 0 };

static void hist_add(term_t *t, const char *cmd) {
    if (!cmd[0]) return;
    int last = (t->hhead + HIST_N - 1) % HIST_N;
    if (t->hcount > 0 && t_eq(t->hist[last], cmd)) return;       /* підряд однакові не дублюємо */
    int i = 0;
    for (; cmd[i] && i < INPUT_MAX; i++) t->hist[t->hhead][i] = cmd[i];
    t->hist[t->hhead][i] = 0;
    t->hhead = (t->hhead + 1) % HIST_N;
    if (t->hcount < HIST_N) t->hcount++;
}

static const char *hist_get(term_t *t, int k) {        /* k = 0 — найновіший */
    return t->hist[(t->hhead + HIST_N - 1 - k) % HIST_N];
}

static void run_command(term_t *t, const char *text) {
    char cmd[INPUT_MAX + 2];
    int n = 0;
    while (*text == ' ') text++;
    for (; text[n] && n < INPUT_MAX; n++) cmd[n] = text[n];
    while (n > 0 && cmd[n - 1] == ' ') n--;
    cmd[n] = 0;
    if (!n) return;

    char word[24];
    int w = 0;
    while (cmd[w] && cmd[w] != ' ' && w < 23) { word[w] = cmd[w]; w++; }
    word[w] = 0;

    if (t_eq(word, "clear") || t_eq(word, "cls")) { term_clear(t); return; }
    if (t_eq(word, "exit")) { gui_window_close(t->win); return; }

    if (t_eq(word, "edit")) {                          /* редактор відкривається окремим вікном */
        const char *arg = cmd + w;
        while (*arg == ' ') arg++;
        if (!*arg) { term_puts(t, "Usage: edit <filename>\n", C_FG); return; }
        char eb[300];
        if (!gui_editor_open(usbfs_edit_path(arg, eb, sizeof eb))) term_puts(t, "edit: no free editor windows\n", 0xFFB04A);
        return;
    }

    for (int i = 0; blocked[i]; i++) {
        if (t_eq(word, blocked[i])) {
            term_puts(t, word, C_FG);
            term_puts(t, ": needs the full screen. Press Exit on the taskbar and run it in the shell.\n", 0xFFB04A);
            return;
        }
    }

    sink_term = t;
    console_set_sink(sink_fn);
    shell_execute_command(cmd);
    console_set_sink(0);
    sink_term = 0;
}

/* ---------------------------------------------------------------- */
/*  Клавіатура                                                       */
/* ---------------------------------------------------------------- */
static void set_line(term_t *t, const char *s) {
    int n = 0;
    for (; s[n] && n < INPUT_MAX; n++) t->line[n] = s[n];
    t->line[n] = 0;
    t->len = t->pos = n;
}

static void term_enter(term_t *t) {
    char cmd[INPUT_MAX + 1];
    int n = 0;
    for (; n < t->len; n++) cmd[n] = t->line[n];
    cmd[n] = 0;

    /* вводимий рядок лишається в історії виводу */
    term_prefix(t);
    term_puts(t, cmd, C_FG);
    term_newline(t);

    t->len = t->pos = 0;
    t->line[0] = 0;
    t->hpos = -1;

    hist_add(t, cmd);
    run_command(t, cmd);
    if (!t->used) return;                 /* команда exit закрила вікно */

    if (t->col > 0) term_newline(t);      /* промпт завжди з нового рядка */
    term_header(t);                       /* ┌──{rose🌹rose}-{dir} — а └─>> малює draw_input */
    t->scroll = 0;
    inv_client(t);
}

static void term_key(gui_window_t *w, const gui_key_t *k, void *ctx) {
    (void)w;
    term_t *t = (term_t *)ctx;
    int code = k->code;

    if (k->ctrl) {
        if (code == 'l' || code == 'L') {
            term_clear(t);
            term_header(t);
            inv_client(t);
        }
        else if (code == 'c' || code == 'C') {
            term_prefix(t);
            term_puts(t, t->line, C_FG);
            term_puts(t, "^C\n", C_FG);
            term_header(t);
            t->len = t->pos = 0; t->line[0] = 0; t->hpos = -1; t->scroll = 0;
            inv_client(t);
        }
        return;
    }

    if (code == GUI_KEY_PGUP || code == GUI_KEY_PGDN) {
        int step = t->rows > 2 ? t->rows - 1 : 1;
        int max = t->cur < TERM_LINES - t->rows ? t->cur : TERM_LINES - t->rows;
        int ns = t->scroll + (code == GUI_KEY_PGUP ? step : -step);
        if (ns < 0) ns = 0;
        if (ns > max) ns = max;
        if (ns != t->scroll) { t->scroll = ns; inv_client(t); }
        return;
    }

    int was_scrolled = t->scroll != 0;
    if (was_scrolled) t->scroll = 0;

    int edited = 0;
    if (code == GUI_KEY_ENTER) {
        term_enter(t);
        return;
    } else if (code == GUI_KEY_BACKSPACE) {
        if (t->pos > 0) {
            for (int i = t->pos - 1; i < t->len; i++) t->line[i] = t->line[i + 1];
            t->pos--; t->len--; edited = 1;
        }
    } else if (code == GUI_KEY_DELETE) {
        if (t->pos < t->len) {
            for (int i = t->pos; i < t->len; i++) t->line[i] = t->line[i + 1];
            t->len--; edited = 1;
        }
    } else if (code == GUI_KEY_LEFT)  { if (t->pos > 0)      { t->pos--; edited = 1; } }
    else if   (code == GUI_KEY_RIGHT) { if (t->pos < t->len) { t->pos++; edited = 1; } }
    else if   (code == GUI_KEY_HOME)  { if (t->pos != 0)     { t->pos = 0; edited = 1; } }
    else if   (code == GUI_KEY_END)   { if (t->pos != t->len){ t->pos = t->len; edited = 1; } }
    else if   (code == GUI_KEY_UP) {
        if (t->hpos + 1 < t->hcount) {
            if (t->hpos < 0) { for (int i = 0; i <= t->len; i++) t->saved[i] = t->line[i]; }
            t->hpos++;
            set_line(t, hist_get(t, t->hpos));
            edited = 1;
        }
    } else if (code == GUI_KEY_DOWN) {
        if (t->hpos >= 0) {
            t->hpos--;
            set_line(t, t->hpos < 0 ? t->saved : hist_get(t, t->hpos));
            edited = 1;
        }
    } else if (code < 256 && cs_printable(code)) {
        if (t->len < INPUT_MAX) {
            for (int i = t->len; i > t->pos; i--) t->line[i] = t->line[i - 1];
            t->line[t->pos++] = (char)code;
            t->len++;
            t->line[t->len] = 0;
            edited = 1;
        }
    }

    if (was_scrolled) inv_client(t);
    else if (edited)  inv_input_row(t);
}

static void term_wheel(gui_window_t *w, int delta, void *ctx) {
    (void)w;
    term_t *t = (term_t *)ctx;
    int max = t->cur < TERM_LINES - t->rows ? t->cur : TERM_LINES - t->rows;
    int ns = t->scroll + delta * 3;
    if (ns < 0) ns = 0;
    if (ns > max) ns = max;
    if (ns != t->scroll) { t->scroll = ns; inv_client(t); }
}

static void term_resize(gui_window_t *w, int cw, int ch, void *ctx) {
    (void)w;
    term_t *t = (term_t *)ctx;
    t->cols = cw / CELL_W > TERM_COLS_MAX ? TERM_COLS_MAX : (cw / CELL_W < 1 ? 1 : cw / CELL_W);
    t->rows = ch / CELL_H < 1 ? 1 : ch / CELL_H;
}

static void term_close(gui_window_t *w, void *ctx) {
    (void)w;
    term_t *t = (term_t *)ctx;
    t->used = 0;
    t->win = 0;
}

static const gui_ops_t term_ops = {
    .paint  = term_paint,
    .resize = term_resize,
    .key    = term_key,
    .wheel  = term_wheel,
    .close  = term_close,
};

/* ---------------------------------------------------------------- */
/*  Відкриття                                                        */
/* ---------------------------------------------------------------- */
gui_window_t *gui_terminal_open(void) {
    int slot = -1, opened = 0;
    for (int i = 0; i < TERM_MAX; i++) {
        if (terms[i].used) opened++;
        else if (slot < 0) slot = i;
    }
    if (slot < 0) return 0;

    gui_window_t *win = gui_window_create(50 + opened * 30, 40 + opened * 30, 660, 420, "Terminal");

    if (win)
        gui_window_set_icon(win, GUI_ICON_TERMINAL);
    if (!win) return 0;

    term_t *t = &terms[slot];
    t->used = 1;
    t->win = win;
    term_clear(t);
    t->len = t->pos = 0;
    t->line[0] = 0;
    t->hcount = t->hhead = 0;
    t->hpos = -1;

    int cw, ch;
    gui_window_client(win, 0, 0, &cw, &ch);
    t->cols = cw / CELL_W > TERM_COLS_MAX ? TERM_COLS_MAX : cw / CELL_W;
    t->rows = ch / CELL_H;

    sink_term = t;
    console_set_sink(sink_fn);
    welcome_to_Merden();
    console_set_sink(0);
    sink_term = 0;
    if (t->col > 0) term_newline(t);
    term_header(t);

    gui_window_set_resizable(win, 1);
    gui_window_set_ops(win, &term_ops, t);
    gui_window_focus(win);
    return win;
}
