
#include <stdint.h>
#include "gui.h"

/* ---------- Налаштування ---------- */

#define C_BG       0x202124
#define C_BTN      0x323436
#define C_OP       0x323436
#define C_EQ       0x42BDF5
#define C_TEXT     0xFFFFFF
#define C_MUTED    0x85888C
#define C_EXPR     0xB8BBC0
#define C_ERR      0xFF7777

#define DEFAULT_W  390
#define DEFAULT_H  590

enum {
    K_DIGIT,
    K_FUNC,
    K_OP,
    K_EQ
};

typedef struct {
    char key;
    const char *label;
    uint8_t col;
    uint8_t row;
    uint8_t kind;
} btn_t;

static const btn_t btns[] = {
    { '%', "%",    0, 0, K_FUNC },
    { 'E', "CE",   1, 0, K_FUNC },
    { 'C', "C",    2, 0, K_FUNC },
    { 'B', "<-",   3, 0, K_FUNC },

    { 'R', "1/x",  0, 1, K_FUNC },
    { 'S', "x2",   1, 1, K_FUNC },
    { 'Q', "sqrt", 2, 1, K_FUNC },
    { '/', "/",    3, 1, K_OP   },

    { '7', "7",    0, 2, K_DIGIT },
    { '8', "8",    1, 2, K_DIGIT },
    { '9', "9",    2, 2, K_DIGIT },
    { '*', "x",    3, 2, K_OP   },

    { '4', "4",    0, 3, K_DIGIT },
    { '5', "5",    1, 3, K_DIGIT },
    { '6', "6",    2, 3, K_DIGIT },
    { '-', "-",    3, 3, K_OP   },

    { '1', "1",    0, 4, K_DIGIT },
    { '2', "2",    1, 4, K_DIGIT },
    { '3', "3",    2, 4, K_DIGIT },
    { '+', "+",    3, 4, K_OP   },

    { 'N', "+/-",  0, 5, K_FUNC },
    { '0', "0",    1, 5, K_DIGIT },
    { '.', ",",    2, 5, K_FUNC },
    { '=', "=",    3, 5, K_EQ   },
};

#define NBTN ((int)(sizeof(btns) / sizeof(btns[0])))

/* ---------- Стан калькулятора ---------- */

typedef struct {
    int used;
    gui_window_t *win;

    int acc;
    int cur;
    char op;

    int typing;
    int err;
    int result_shown;

    /* Останній розмір клієнтської області */
    int last_w;
    int last_h;
} calc_t;

static calc_t calc;

/* ---------- Геометрія ---------- */

typedef struct {
    int pad;
    int gap;

    int disp_h;
    int mem_h;
    int grid_y;

    int bw;
    int bh;
} calc_layout_t;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static calc_layout_t calc_layout(int cw, int ch)
{
    calc_layout_t l;

    if (cw < 1) cw = DEFAULT_W;
    if (ch < 1) ch = DEFAULT_H;

    l.pad = clamp_int(cw / 45, 4, 12);
    l.gap = clamp_int(l.pad / 2, 2, 6);

    /*
     * Висота дисплея залежить від висоти вікна.
     * При малому розмірі дисплей займає менше місця.
     */
    l.disp_h = clamp_int(ch / 4, 72, 160);
    l.mem_h = clamp_int(ch / 24, 18, 30);

    l.grid_y = l.pad + l.disp_h + l.mem_h + l.pad;

    l.bw = (cw - 2 * l.pad - 3 * l.gap) / 4;
    l.bh = (ch - l.grid_y - l.pad - 5 * l.gap) / 6;

    /*
     * Захист від нульових та від'ємних розмірів.
     */
    if (l.bw < 1) l.bw = 1;
    if (l.bh < 1) l.bh = 1;

    return l;
}

static int btn_x(const calc_layout_t *l, const btn_t *b)
{
    return l->pad + b->col * (l->bw + l->gap);
}

static int btn_y(const calc_layout_t *l, const btn_t *b)
{
    return l->grid_y + b->row * (l->bh + l->gap);
}

/* ---------- Числа ---------- */

static int int_to_str(int v, char *buf)
{
    unsigned u = v < 0
        ? 0u - (unsigned)v
        : (unsigned)v;

    char tmp[12];
    int n = 0;
    int p = 0;

    do {
        tmp[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);

    if (v < 0)
        buf[p++] = '-';

    while (n)
        buf[p++] = tmp[--n];

    buf[p] = 0;
    return p;
}

static int int_sqrt(int n)
{
    if (n <= 0)
        return 0;

    int x = n;
    int y = x / 2 + 1;

    while (y < x) {
        x = y;
        y = x / 2 + n / (2 * x);

        /*
         * Перевірка достатня для цілочисельного
         * квадратного кореня в нашому діапазоні.
         */
        if (y >= x)
            break;
    }

    /*
     * Корекція результату без множення x*x,
     * яке могло б переповнити int.
     */
    while (x > n / x)
        x--;

    while (x + 1 > 0 && x + 1 <= n / (x + 1))
        x++;

    return x;
}

/* ---------- Арифметика ---------- */

static int eval(int a, int b, char op)
{
    switch (op) {
    case '+':
        return (int)((unsigned)a + (unsigned)b);

    case '-':
        return (int)((unsigned)a - (unsigned)b);

    case '*':
        return (int)((unsigned)a * (unsigned)b);

    case '/':
        if (b == 0) {
            calc.err = 1;
            return 0;
        }

        /* Уникаємо signed overflow: INT_MIN / -1. */
        if (a == (-2147483647 - 1) && b == -1)
            return a;

        return a / b;
    }

    return b;
}

static void clear_all(void)
{
    calc.acc = 0;
    calc.cur = 0;
    calc.op = 0;

    calc.typing = 0;
    calc.err = 0;
    calc.result_shown = 0;
}

static void press(char k)
{
    if (k == 'C') {
        clear_all();
        return;
    }

    if (calc.err)
        clear_all();

    /* Цифри */
    if (k >= '0' && k <= '9') {
        if (!calc.typing || calc.result_shown) {
            calc.cur = 0;
            calc.typing = 1;
            calc.result_shown = 0;
        }

        if (calc.cur >= 0 && calc.cur <= 214748364)
            calc.cur = calc.cur * 10 + (k - '0');
        else if (calc.cur < 0 && calc.cur >= -214748364)
            calc.cur = calc.cur * 10 - (k - '0');

        return;
    }

    /* Clear Entry */
    if (k == 'E') {
        calc.cur = 0;
        calc.typing = 0;
        calc.result_shown = 0;
        return;
    }

    /* Backspace */
    if (k == 'B') {
        if (calc.typing) {
            calc.cur /= 10;
        }
        return;
    }

    /* Зміна знака */
    if (k == 'N') {
        calc.cur = (int)(0u - (unsigned)calc.cur);
        calc.typing = 1;
        calc.result_shown = 0;
        return;
    }

    /* Обернене число: цілочисельна арифметика */
    if (k == 'R') {
        if (calc.cur == 0) {
            calc.err = 1;
        } else {
            calc.cur = 1 / calc.cur;
        }

        calc.typing = 0;
        calc.result_shown = 1;
        return;
    }

    /* Квадрат */
    if (k == 'S') {
        calc.cur = (int)(
            (unsigned)calc.cur * (unsigned)calc.cur
        );

        calc.typing = 0;
        calc.result_shown = 1;
        return;
    }

    /* Квадратний корінь */
    if (k == 'Q') {
        if (calc.cur < 0) {
            calc.err = 1;
        } else {
            calc.cur = int_sqrt(calc.cur);
        }

        calc.typing = 0;
        calc.result_shown = 1;
        return;
    }

    /* Відсоток від першого операнда */
    if (k == '%') {
        calc.cur = (int)(
            ((unsigned)calc.acc * (unsigned)calc.cur) / 100u
        );

        calc.typing = 0;
        calc.result_shown = 1;
        return;
    }

    /* Десяткова кома поки не підтримується */
    if (k == '.')
        return;

    /* Результат */
    if (k == '=') {
        if (calc.op && calc.typing)
            calc.cur = eval(calc.acc, calc.cur, calc.op);
        else if (calc.op)
            calc.cur = calc.acc;

        calc.acc = calc.cur;
        calc.op = 0;
        calc.typing = 0;
        calc.result_shown = 1;
        return;
    }

    /* Арифметичні операції */
    if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (calc.op && calc.typing)
            calc.acc = eval(calc.acc, calc.cur, calc.op);
        else if (!calc.op)
            calc.acc = calc.cur;

        calc.cur = calc.acc;
        calc.op = k;
        calc.typing = 0;
        calc.result_shown = 0;
    }
}

/* ---------- Малювання тексту ---------- */

static int text_length(const char *s)
{
    int n = 0;

    while (s[n])
        n++;

    return n;
}

static void draw_scaled_text(
    int x, int y,
    const char *s,
    uint32_t color,
    int scale)
{
    if (scale < 1)
        scale = 1;

    for (int i = 0; s[i]; i++) {
        gui_draw_char_scaled(
            x + i * 8 * scale,
            y,
            s[i],
            color,
            scale
        );
    }
}

static void draw_centered_text(
    int x, int y, int w, int h,
    const char *s,
    uint32_t color)
{
    int n = text_length(s);

    int scale = clamp_int(w / ((n ? n : 1) * 8 + 8), 1, 2);

    int tw = n * 8 * scale;
    int th = 8 * scale;

    int tx = x + (w - tw) / 2;
    int ty = y + (h - th) / 2;

    draw_scaled_text(tx, ty, s, color, scale);
}

/* ---------- Малювання інтерфейсу ---------- */

static void calc_paint(
    gui_window_t *w,
    int cx, int cy, int cw, int ch,
    void *ctx)
{
    (void)w;
    (void)ctx;

    calc.last_w = cw;
    calc.last_h = ch;

    calc_layout_t l = calc_layout(cw, ch);

    gui_draw_rect(cx, cy, cw, ch, C_BG);

    /* Дисплей */
    int dx = cx + l.pad;
    int dy = cy + l.pad;
    int dw = cw - 2 * l.pad;

    gui_draw_rrect(dx, dy, dw, l.disp_h, C_BG);

    /* Вираз над результатом */
    if (calc.op && !calc.err) {
        char expr[24];

        int n = int_to_str(calc.acc, expr);

        expr[n++] = ' ';
        expr[n++] = calc.op == '*' ? 'x' : calc.op;
        expr[n] = 0;

        gui_draw_text(
            dx + dw - 12 - 8 * n,
            dy + 8,
            expr,
            C_EXPR
        );
    }

    /* Велике число */
    char buf[16];
    int len;
    uint32_t number_color = C_TEXT;

    if (calc.err) {
        buf[0] = 'E';
        buf[1] = 'r';
        buf[2] = 'r';
        buf[3] = 'o';
        buf[4] = 'r';
        buf[5] = 0;

        len = 5;
        number_color = C_ERR;
    } else {
        len = int_to_str(calc.cur, buf);
    }

    int available_w = dw - 20;
    int scale = clamp_int(
        available_w / ((len ? len : 1) * 8),
        1,
        4
    );

    int number_w = len * 8 * scale;
    int number_h = 8 * scale;

    int number_x = dx + dw - 12 - number_w;
    int number_y = dy + l.disp_h - number_h - 14;

    if (number_x < dx + 4)
        number_x = dx + 4;

    if (number_y < dy + 30)
        number_y = dy + 30;

    draw_scaled_text(
        number_x,
        number_y,
        buf,
        number_color,
        scale
    );

    /* Панель пам'яті */
    static const char *mem_labels[] = {
        "MC", "MR", "M+", "M-", "MS", "Mv"
    };

    int mem_y = cy + l.pad + l.disp_h + 5;
    int mem_step = cw / 6;

    for (int i = 0; i < 6; i++) {
        int n = text_length(mem_labels[i]);

        gui_draw_text(
            cx + i * mem_step + (mem_step - n * 8) / 2,
            mem_y,
            mem_labels[i],
            C_MUTED
        );
    }

    /* Кнопки */
    for (int i = 0; i < NBTN; i++) {
        const btn_t *b = &btns[i];

        int x = cx + btn_x(&l, b);
        int y = cy + btn_y(&l, b);

        uint32_t face = C_BTN;

        if (b->kind == K_OP)
            face = C_OP;

        if (b->kind == K_EQ)
            face = C_EQ;

        gui_draw_rrect(x, y, l.bw, l.bh, face);

        draw_centered_text(
            x, y,
            l.bw, l.bh,
            b->label,
            C_TEXT
        );
    }
}

/* ---------- Обробка миші ---------- */

static void calc_click(
    gui_window_t *w,
    int x, int y,
    void *ctx)
{
    (void)ctx;

    /*
     * Беремо останній реальний розмір клієнтської
     * області, отриманий у calc_paint().
     */
    calc_layout_t l = calc_layout(
        calc.last_w,
        calc.last_h
    );

    for (int i = 0; i < NBTN; i++) {
        const btn_t *b = &btns[i];

        int bx = btn_x(&l, b);
        int by = btn_y(&l, b);

        if (x >= bx && x < bx + l.bw &&
            y >= by && y < by + l.bh) {
            press(b->key);
            gui_window_invalidate_client(w);
            return;
        }
    }
}

/* ---------- Обробка клавіатури ---------- */

static void calc_key(
    gui_window_t *w,
    const gui_key_t *k,
    void *ctx)
{
    (void)ctx;

    int code = k->code;

    if ((code >= '0' && code <= '9') ||
        code == '+' || code == '-' ||
        code == '*' || code == '/' ||
        code == '%') {
        press((char)code);
    } else if (code == '=' || code == GUI_KEY_ENTER) {
        press('=');
    } else if (code == 'c' || code == 'C' ||
               code == GUI_KEY_ESC ||
               code == GUI_KEY_DELETE) {
        press('C');
    } else if (code == GUI_KEY_BACKSPACE) {
        press('B');
    } else {
        return;
    }

    gui_window_invalidate_client(w);
}

/* ---------- Закриття ---------- */

static void calc_close(gui_window_t *w, void *ctx)
{
    (void)w;
    (void)ctx;

    calc.used = 0;
    calc.win = 0;
}

/* ---------- GUI ---------- */

static const gui_ops_t calc_ops = {
    .paint = calc_paint,
    .key   = calc_key,
    .click = calc_click,
    .close = calc_close,
};

gui_window_t *gui_calc_open(void)
{
    if (calc.used) {
        gui_window_focus(calc.win);
        return calc.win;
    }

    gui_window_t *win = gui_window_create(
        140, 40,
        402, 610,
        "Calculator"
    );

    if (!win)
        return 0;

    gui_window_set_icon(win, GUI_ICON_CALC);

    calc.used = 1;
    calc.win = win;

    calc.last_w = DEFAULT_W;
    calc.last_h = DEFAULT_H;

    clear_all();

    gui_window_set_ops(win, &calc_ops, &calc);
    gui_window_focus(win);

    return win;
}
