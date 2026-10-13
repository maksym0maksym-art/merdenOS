#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/*
 * Єдиний інтерфейс мишки для GUI. Під капотом: USB (UHCI, boot protocol),
 * а якщо її нема — PS/2 (QEMU за замовчуванням, VirtualBox, ноутбучні тачпади, BIOS-емуляція).
 *
 *   mouse_init();                          // один раз
 *   mouse_cursor_show();                   // (за бажанням) програмний курсор
 *   for (;;) {
 *       if (mouse_poll()) {                // НЕ блокує; викликай у головному циклі
 *           const mouse_state_t *m = mouse_get();
 *           if (m->pressed & MOUSE_BTN_LEFT) click(m->x, m->y);
 *       }
 *   }
 */

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

typedef enum {
    MOUSE_BACKEND_NONE = 0,
    MOUSE_BACKEND_USB  = 1,
    MOUSE_BACKEND_PS2  = 2,
    MOUSE_BACKEND_I2C  = 3,     /* тачпад або миша по I2C HID (сучасні ноутбуки) */
    MOUSE_BACKEND_XHCI = 4      /* USB-миша через xHCI (сучасні ноутбуки) */
} mouse_backend_t;

typedef struct {
    int32_t x, y;        /* позиція курсора в пікселях, завжди в межах екрана */
    int32_t dx, dy;      /* зсув, накопичений за останній mouse_poll() */
    int32_t wheel;       /* колесо за останній mouse_poll(); > 0 — вгору (від себе) */
    uint8_t buttons;     /* кнопки, що затиснуті зараз */
    uint8_t pressed;     /* кнопки, натиснуті за останній mouse_poll() (фронт) */
    uint8_t released;    /* кнопки, відпущені за останній mouse_poll() */
} mouse_state_t;

int  mouse_init(void);                       /* 1 — мишку знайдено */
int  mouse_poll(void);                       /* ненульове, якщо щось змінилось */
const mouse_state_t *mouse_get(void);
mouse_backend_t mouse_backend(void);

void mouse_set_bounds(int32_t width, int32_t height);   /* за замовчуванням — розмір фреймбуфера */
void mouse_set_pos(int32_t x, int32_t y);

void mouse_cursor_set_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y);

void mouse_cursor_show(void);
void mouse_cursor_hide(void);
int  mouse_cursor_visible(void);
/* Куди зараз намальовано курсор. 0 — він прихований. Потрібно GUI, щоб не ховати його даремно. */
int  mouse_cursor_bounds(int *x, int *y, int *w, int *h);

/* --- PS/2 (внутрішнє, але keyboard.c викликає ps2_mouse_feed) --- */
int  ps2_mouse_init(void);
void ps2_mouse_pump(void);                   /* забрати з контролера байти, що належать мишці */
void ps2_mouse_feed(uint8_t byte);           /* один байт від мишки */
int  ps2_mouse_read(int *dx, int *dy, int *wheel, uint8_t *buttons);   /* наступний готовий пакет */

#endif