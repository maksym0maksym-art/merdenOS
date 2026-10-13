#include <stdint.h>
#include "gui.h"

/* Цілочисельний калькулятор: заокруглені кнопки, великий дисплей, миша + клавіатура. */

#define PAD    10
#define GAP    8
#define BW     56
#define BH     40
#define DISP_H 78
#define GRID_Y (PAD + DISP_H + PAD)

#define C_BG        0x23244A
#define C_DISP      0x15162E
#define C_EXPR      0x8E91C9
#define C_NUM       0xFFFFFF
#define C_ERR       0xFF7B7B

enum { K_DIGIT, K_FUNC, K_OP, K_EQ };

typedef struct { char key; const char *label; uint8_t col, row, span, kind; } btn_t;

static const btn_t btns[] = {
    { 'C', "C",   0, 0, 1, K_FUNC }, { 'N', "+/-", 1, 0, 1, K_FUNC },
    { 'B', "<-",  2, 0, 1, K_FUNC }, { '/', "/",   3, 0, 1, K_OP   },
    { '7', "7",   0, 1, 1, K_DIGIT }, { '8', "8",  1, 1, 1, K_DIGIT },
    { '9', "9",   2, 1, 1, K_DIGIT }, { '*', "x",  3, 1, 1, K_OP   },
    { '4', "4",   0, 2, 1, K_DIGIT }, { '5', "5",  1, 2, 1, K_DIGIT },
    { '6', "6",   2, 2, 1, K_DIGIT }, { '-', "-",  3, 2, 1, K_OP   },
    { '1', "1",   0, 3, 1, K_DIGIT }, { '2', "2",  1, 3, 1, K_DIGIT },
    { '3', "3",   2, 3, 1, K_DIGIT }, { '+', "+",  3, 3, 1, K_OP   },
    { '0', "0",   0, 4, 2, K_DIGIT }, { '=', "=",  2, 4, 2, K_EQ   },
};
#define NBTN ((int)(sizeof(btns) / sizeof(btns[0])))

typedef struct {
    int used;
    gui_window_t *win;
    int acc, cur;
    char op;
    int typing;
    int err;
} calc_t;

static calc_t calc;

/* ---------- логіка ---------- */

static int eval(int a, int b, char op) {
    switch (op) {
    case '+': return (int)((unsigned)a + (unsigned)b);
    case '-': return (int)((unsigned)a - (unsigned)b);
    case '*': return (int)((unsigned)a * (unsigned)b);
    case '/':
        if (b == 0) { calc.err = 1; return 0; }
        if (b == -1) return (int)(0u - (unsigned)a);     /* без #DE на INT_MIN / -1 */
        return a / b;
    }
    return b;
}

static void clear_all(void) {
    calc.acc = calc.cur = 0;
    calc.op = 0;
    calc.typing = 0;
    calc.err = 0;
}

static void press(char k) {
    if (k == 'C') { clear_all(); return; }
    if (calc.err) clear_all();

    if (k >= '0' && k <= '9') {
        if (!calc.typing) { calc.cur = 0; calc.typing = 1; }
        if (calc.cur < 100000000) calc.cur = calc.cur * 10 + (k - '0');
        return;
    }

    if (k == 'B') { if (calc.typing) calc.cur /= 10; return; }

    if (k == 'N') {                                       /* +/- */
        if (calc.typing || !calc.op) calc.cur = (int)(0u - (unsigned)calc.cur);
        return;
    }

    if (k == '=') {
        if (calc.op && calc.typing) calc.acc = eval(calc.acc, calc.cur, calc.op);
        else if (calc.op)           calc.acc = calc.cur;
        if (calc.op) calc.cur = calc.acc;
        calc.op = 0;
        calc.typing = 0;
        return;
    }

    /* + - * / */
    if (calc.op && calc.typing) calc.acc = eval(calc.acc, calc.cur, calc.op);
    else if (!calc.op)          calc.acc = calc.cur;
    calc.cur = calc.acc;
    calc.op = k;
    calc.typing = 0;
}

static int int_to_str(int v, char *buf) {
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    char tmp[12];
    int n = 0, p = 0;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) buf[p++] = '-';
    while (n) buf[p++] = tmp[--n];
    buf[p] = 0;
    return p;
}

/* ---------- вигляд ---------- */

static int btn_x(const btn_t *b) { return PAD + b->col * (BW + GAP); }
static int btn_y(const btn_t *b) { return GRID_Y + b->row * (BH + GAP); }
static int btn_w(const btn_t *b) { return b->span * BW + (b->span - 1) * GAP; }

static void calc_paint(gui_window_t *w, int cx, int cy, int cw, int ch, void *ctx) {
    (void)w; (void)ctx;
    gui_draw_rect(cx, cy, cw, ch, C_BG);

    /* дисплей */
    int dx = cx + PAD, dy = cy + PAD, dw = cw - 2 * PAD;
    gui_draw_rrect(dx, dy, dw, DISP_H, C_DISP);

    if (calc.op && !calc.err) {                           /* «12 +» дрібно зверху */
        char e[24];
        int n = int_to_str(calc.acc, e);
        e[n++] = ' ';
        e[n++] = calc.op == '*' ? 'x' : calc.op;
        e[n] = 0;
        gui_draw_text(dx + dw - 12 - 8 * n, dy + 8, e, C_EXPR);
    }

    char buf[16];
    uint32_t col = C_NUM;
    int len;
    if (calc.err) {
        buf[0] = 'E'; buf[1] = 'r'; buf[2] = 'r'; buf[3] = 'o'; buf[4] = 'r'; buf[5] = 0;
        len = 5;
        col = C_ERR;
    } else {
        len = int_to_str(calc.cur, buf);
    }
    int nx = dx + dw - 12 - 16 * len, ny = dy + 30;
    for (int i = 0; i < len; i++) gui_draw_char_scaled(nx + 16 * i, ny, buf[i], col, 2);

    /* кнопки */
    for (int i = 0; i < NBTN; i++) {
        const btn_t *b = &btns[i];
        int x = cx + btn_x(b), y = cy + btn_y(b), w2 = btn_w(b);

        uint32_t face, shade;
        switch (b->kind) {
        case K_FUNC: face = 0x555A9E; shade = 0x3C4075; break;
        case K_OP:   face = 0x5B66D6; shade = 0x424CA8; break;
        case K_EQ:   face = 0x3DBE86; shade = 0x2A8F63; break;
        default:     face = 0x3B3D7A; shade = 0x2A2B58; break;
        }
        if (b->kind == K_OP && !calc.typing && calc.op == b->key) { face = 0x8A94F2; shade = 0x5B66D6; }

        gui_draw_rrect(x, y + 3, w2, BH - 3, shade);      /* «товщина» кнопки */
        gui_draw_rrect(x, y, w2, BH - 3, face);

        int n = 0;
        while (b->label[n]) n++;
        gui_draw_text(x + (w2 - 8 * n) / 2, y + (BH - 3 - 18) / 2, b->label, 0xFFFFFF);
    }
}

static void calc_click(gui_window_t *w, int x, int y, void *ctx) {
    (void)ctx;
    for (int i = 0; i < NBTN; i++) {
        const btn_t *b = &btns[i];
        int bx = btn_x(b), by = btn_y(b);
        if (x >= bx && x < bx + btn_w(b) && y >= by && y < by + BH) {
            press(b->key);
            gui_window_invalidate_client(w);
            return;
        }
    }
}

static void calc_key(gui_window_t *w, const gui_key_t *k, void *ctx) {
    (void)ctx;
    int code = k->code;
    if ((code >= '0' && code <= '9') || code == '+' || code == '-' || code == '*' || code == '/')
        press((char)code);
    else if (code == '=' || code == GUI_KEY_ENTER) press('=');
    else if (code == 'c' || code == 'C' || code == GUI_KEY_ESC || code == GUI_KEY_DELETE) press('C');
    else if (code == GUI_KEY_BACKSPACE) press('B');
    else return;
    gui_window_invalidate_client(w);
}

static void calc_close(gui_window_t *w, void *ctx) {
    (void)w; (void)ctx;
    calc.used = 0;
    calc.win = 0;
}

static const gui_ops_t calc_ops = {
    .paint = calc_paint,
    .key   = calc_key,
    .click = calc_click,
    .close = calc_close,
};

gui_window_t *gui_calc_open(void) {
    if (calc.used) { gui_window_focus(calc.win); return calc.win; }   /* один екземпляр */

    gui_window_t *win = gui_window_create(140, 70, 270, 366, "Calculator");
    if (win)
        gui_window_set_icon(win, GUI_ICON_CALC);
    if (!win) return 0;

    calc.used = 1;
    calc.win = win;
    clear_all();

    gui_window_set_ops(win, &calc_ops, &calc);
    gui_window_focus(win);
    return win;
}