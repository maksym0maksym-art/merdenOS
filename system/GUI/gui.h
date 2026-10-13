#ifndef GUI_H
#define GUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Параметри ---------- */

#define GUI_MAX_WINDOWS   16
#define GUI_MAX_WIDGETS   16

#define GUI_TITLE_H       24   /* висота заголовка вікна */
/* 1 — панель завдань зверху, 0 — знизу */
#define GUI_TASKBAR_TOP   1
#define GUI_TASKBAR_H     44   /* >= 42, щоб іконка «Пуск» малювалась у масштабі x2 */

/* ---------- Тайлінг (як Hyprland / dwindle) ----------
 * GUI_TILING   1 — нові вікна автоматично діляться по екрану, 0 — звичайні плаваючі вікна
 * GUI_TILE_GAP відстань між вікнами, px  (~4 мм на моніторі ~96 dpi ≈ 14-15 px)
 * GUI_TILE_OUT відступ від країв екрана / панелі завдань, px */
#define GUI_TILING        1
#define GUI_TILE_GAP      14
#define GUI_TILE_OUT      14

/* Прозорість вікна терміналу (0..255, 255 — непрозоре). Застосовується автоматично,
 * коли вікну ставлять значок GUI_ICON_TERMINAL; 255 — вимкнути. */
#define GUI_TERMINAL_ALPHA 217   /* 85% непрозорості */

/* ---------- Робочі столи (workspaces) ----------
 * Alt+1..9 — перейти на стіл, Alt+Shift+1..9 — відправити активне вікно на стіл.
 * Клік по цифрі на панелі завдань теж перемикає стіл. */
#define GUI_WORKSPACES    9

/* ---------- Кольоровий смайлик-троянда ----------
 * Малюється як один 16x18 значок з двох сусідніх символів (по 8 px кожен), тому терміналу
 * нічого особливого робити не треба: достатньо надрукувати ці два символи підряд.
 *   printf("rose" GUI_ROSE_STR "rose");   -> rose<троянда>rose */
#define GUI_CH_ROSE_L     0x01
#define GUI_CH_ROSE_R     0x02
#define GUI_ROSE_STR      "\x01\x02"

/* ---------- Рамкові символи для промта у стилі Kali ----------
 * Малюються кольором тексту, з'єднуються між собою і між рядками без проміжків:
 *   ┌──{...}-{...}
 *   └─>>                                                                        */
#define GUI_CH_BOX_TL     0x03        /* ┌ */
#define GUI_CH_BOX_H      0x04        /* ─ */
#define GUI_CH_BOX_BL     0x05        /* └ */
#define GUI_BOX_TL_STR    "\x03"
#define GUI_BOX_H_STR     "\x04"
#define GUI_BOX_BL_STR    "\x05"

/* ---------- Коди клавіш ----------
 * Звичайні символи передаються як є (код ASCII), спеціальні клавіші
 * починаються з 0x100, щоб не перетинатися з символами. */

enum {
    GUI_KEY_BACKSPACE = 0x100,
    GUI_KEY_ENTER,
    GUI_KEY_TAB,
    GUI_KEY_ESC,
    GUI_KEY_F4,
    GUI_KEY_UP,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_HOME,
    GUI_KEY_END,
    GUI_KEY_DELETE,
    GUI_KEY_PGUP,
    GUI_KEY_PGDN,
    GUI_KEY_SUPER        /* Win/Super натиснули й відпустили без інших клавіш */
};

/* ---------- Типи ---------- */

typedef struct gui_window gui_window_t;

typedef struct {
    int     code;    /* ASCII-символ або GUI_KEY_* */
    uint8_t ctrl;
    uint8_t alt;
    uint8_t shift;
} gui_key_t;

/* Колбек кнопки */
typedef void (*gui_click_fn)(void *ctx);

/* Обробники подій вікна; будь-який можна лишити NULL.
 * paint:     (cx, cy) — верхній лівий кут клієнтської області на екрані,
 *            (cw, ch) — її розмір. Малювати через gui_draw_*.
 * resize:    нові розміри клієнтської області (викликається і тайлінгом).
 * click:     координати відносно клієнтської області.
 * wheel:     delta від миші (знак — напрямок).
 * close:     викликається вже після видалення вікна з пулу.
 * can_close: повернути 0, щоб заборонити закриття. */
typedef struct {
    void (*paint)(gui_window_t *win, int cx, int cy, int cw, int ch, void *ctx);
    void (*resize)(gui_window_t *win, int cw, int ch, void *ctx);
    void (*key)(gui_window_t *win, const gui_key_t *k, void *ctx);
    void (*click)(gui_window_t *win, int x, int y, void *ctx);
    void (*wheel)(gui_window_t *win, int delta, void *ctx);
    void (*close)(gui_window_t *win, void *ctx);
    int  (*can_close)(gui_window_t *win, void *ctx);
    /* drag:    миша рухається з натиснутою лівою кнопкою (після click у цьому вікні);
     *          x, y — відносно клієнтської області, можуть виходити за її межі.
     * release: ліву кнопку відпущено (викликається, якщо був click у цьому вікні). */
    void (*drag)(gui_window_t *win, int x, int y, void *ctx);
    void (*release)(gui_window_t *win, int x, int y, void *ctx);
} gui_ops_t;

/* ---------- Життєвий цикл ---------- */

int  gui_init(void);                 /* 1 — успіх, 0 — немає/непідтримуваний фреймбуфер */
void gui_frame(void);                /* один крок: ввід + перемальовування */
void gui_run(void);                  /* цикл, поки не буде gui_quit() */
void gui_quit(void);
void gui_set_launcher(void (*fn)(void));   /* Ctrl+Alt+T (і «Пуск», якщо в меню немає пунктів) */

/* Меню «Пуск»: пункти додаються після gui_init() */
#define GUI_MENU_MAX 8
int  gui_menu_add(const char *label, void (*fn)(void));   /* індекс або -1 */
enum { GUI_ICON_APP = 0, GUI_ICON_TERMINAL, GUI_ICON_EDITOR, GUI_ICON_CALC, GUI_ICON_POWER, GUI_ICON_PAINT, GUI_ICON_FOLDER };
int  gui_menu_add_ex(const char *label, int icon, void (*fn)(void));

/* BMP 24 bpp без стиснення; масштабується з обрізанням під розмір екрана */
int  gui_set_wallpaper_bmp(const uint8_t *data, uint32_t size);

/* ---------- Перемальовування ---------- */

void gui_invalidate_rect(int x, int y, int w, int h);
void gui_invalidate_all(void);

/* ---------- Вікна ---------- */

/* Якщо GUI_TILING == 1, то x, y, w, h — лише «бажана» геометрія для плаваючого режиму;
 * реальний розмір і позицію вікна задає тайлінг (ops->resize повідомить про розмір). */
gui_window_t *gui_window_create(int x, int y, int w, int h, const char *title);
void gui_window_close(gui_window_t *win);            /* без перевірки can_close */
void gui_window_request_close(gui_window_t *win);    /* з перевіркою can_close */

void gui_window_focus(gui_window_t *win);
void gui_window_minimize(gui_window_t *win);
void gui_window_move(gui_window_t *win, int x, int y);          /* ігнорується для тайлових вікон */
void gui_window_resize(gui_window_t *win, int w, int h);        /* ігнорується для тайлових вікон */
void gui_window_toggle_maximize(gui_window_t *win);
void gui_window_set_resizable(gui_window_t *win, int resizable);
void gui_window_set_icon(gui_window_t *win, int icon);        /* GUI_ICON_*: значок у панелі завдань */
void gui_window_set_ops(gui_window_t *win, const gui_ops_t *ops, void *ctx);

/* Плаваюче вікно (поза тайлінгом): 1 — плаває, 0 — бере участь у тайлінгу.
 * Alt+F перемикає це для активного вікна. */
void gui_window_set_floating(gui_window_t *win, int floating);
void gui_window_toggle_floating(gui_window_t *win);

/* Робочі столи: 0 .. GUI_WORKSPACES-1 */
void gui_workspace_switch(int ws);
void gui_workspace_step(int dir);        /* +1 — наступний стіл, -1 — попередній (без зациклення) */
int  gui_workspace_current(void);
void gui_window_move_to_workspace(gui_window_t *win, int ws);

/* Прозорість клієнтської області: 255 — непрозоре, менше — напівпрозоре (рамка й заголовок лишаються суцільними) */
void gui_window_set_alpha(gui_window_t *win, int alpha);

void gui_window_invalidate(gui_window_t *win);
void gui_window_invalidate_client(gui_window_t *win);
void gui_window_invalidate_title(gui_window_t *win);

/* Екранні координати та розмір клієнтської області; будь-який покажчик може бути NULL */
void gui_window_client(const gui_window_t *win, int *x, int *y, int *cw, int *ch);

/* Вікна-програми */
gui_window_t *gui_terminal_open(void);
gui_window_t *gui_editor_open(const char *filename);
gui_window_t *gui_calc_open(void);
gui_window_t *gui_explorer_open(void);

/* ---------- Віджети ---------- */

/* Повертають індекс віджета або -1, якщо місця немає.
 * Рядок text не копіюється: він має жити, поки живе вікно. */
int gui_add_label(gui_window_t *win, int x, int y, const char *text, uint32_t color);
int gui_add_button(gui_window_t *win, int x, int y, int w, int h, const char *text,
                   gui_click_fn fn, void *ctx);

/* ---------- Малювання (лише всередині paint) ---------- */

void gui_draw_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_char(int x, int y, char ch, uint32_t color);
void gui_draw_text(int x, int y, const char *s, uint32_t color);
void gui_draw_rrect(int x, int y, int w, int h, uint32_t color);                /* із заокругленими кутами */
void gui_draw_char_scaled(int x, int y, char ch, uint32_t color, int scale);    /* символ у scale разів більший */
void gui_get_clip(int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* GUI_H */