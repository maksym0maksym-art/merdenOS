#ifndef TOUCHPAD_H
#define TOUCHPAD_H

#include <stdint.h>
#include "i2c_hid.h"


int  tp_init(void);                                   /* 1 — тачпад (або I2C-мишу) знайдено */
int  tp_poll(int *dx, int *dy, int *wheel, uint8_t *buttons);   /* НЕ блокує. 1 — є подія */
void tp_set_speed(int pct);                           /* швидкість: % ширини екрана на всю ширину тачпада (5..200, типово 40) */
int  tp_get_speed(void);
void tp_set_screen(int w, int h);                     /* під розмір екрана підбирається швидкість */
int  tp_kind(void);
void tp_mode_info(int *verified, int *ok, int *sent, uint32_t *n_touch, uint32_t *n_mouse);   /* діагностика режиму PTP */
int  tp_fingers(void);                                /* скільки пальців у останньому звіті (для тесту `mouse`) */

/* нижче — для тестів */
void tp_reset(int kind, int xmin, int xmax, int ymin, int ymax);
void tp_feed(const hid_frame_t *f, uint64_t now);
void tp_tick(uint64_t now);
int  tp_pop(int *dx, int *dy, int *wheel, uint8_t *buttons);

#endif
