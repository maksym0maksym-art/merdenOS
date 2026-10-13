#ifndef I2C_HID_H
#define I2C_HID_H

#include <stdint.h>
#include "i2c_dw.h"

/*
 * HID over I2C (специфікація Microsoft). Так підключені майже всі тачпади сучасних ноутбуків.
 * Модуль сам знаходить пристрій (без ACPI): перебирає I2C-контролери й адреси та
 * перевіряє, чи відповідає там HID-дескриптор.
 */

#define HID_MAX_CONTACTS 10
#define HID_MAX_FIELDS   192
#define HID_MAX_IN       256
#define HID_MAX_RDESC    4096

enum { HID_KIND_NONE = 0, HID_KIND_TOUCHPAD = 1, HID_KIND_MOUSE = 2 };

typedef struct {
    uint8_t  rid;               /* ID звіту (0 — звіти без ID) */
    uint8_t  kind;              /* 1 — Input, 2 — Feature */
    uint8_t  group;             /* номер "пальця" (логічна колекція Finger); 0 — поза пальцями */
    uint8_t  relative;
    uint16_t page, usage;
    uint16_t off;               /* зсув у бітах від початку даних звіту (після байта ID) */
    uint8_t  size;              /* розмір у бітах */
    int32_t  lmin, lmax;
} hid_field_t;

typedef struct {
    hid_field_t f[HID_MAX_FIELDS];
    int nf;
    int has_ids;                /* чи є в дескрипторі Report ID */
    uint16_t feature_bits[256]; /* розмір Feature-звіту за ID */
} hid_desc_t;

typedef struct { int id, x, y; } hid_contact_t;

typedef struct {
    int kind;                   /* HID_KIND_* */
    /* тачпад: пальці, що торкаються (з підтвердженням, не долоня) */
    int n;
    hid_contact_t c[HID_MAX_CONTACTS];
    int button;                 /* биті 0..2: кнопки під тачпадом / мишки */
    /* миша: зсув */
    int dx, dy, wheel;
} hid_frame_t;

typedef struct {
    dw_i2c_t ctl[DW_MAX_CTRL];
    int nctl;

    dw_i2c_t *c;                /* контролер, на якому знайшли пристрій */
    uint8_t   addr;
    uint16_t  reg_cmd, reg_data, reg_input, reg_report;
    uint16_t  max_in;
    uint16_t  vid, pid;

    hid_desc_t desc;
    int      kind;
    int      xmin, xmax, ymin, ymax;     /* логічні межі координат тачпада */

    /* неблокуюче опитування */
    uint8_t  inbuf[HID_MAX_IN];
    int      reading;
    uint64_t next_poll;
    int      errors;

    /* діагностика режиму Precision Touchpad */
    int      mode_sent, mode_ok;         /* скільки Feature-звітів надіслано / прийнято */
    int      mode_verified;              /* 1 — Input Mode прочитано назад і він = 3 */
    uint32_t n_touch, n_mouse;           /* скільки кадрів-тачпада / кадрів-миші прийшло */
    uint64_t next_retry;
    int      retries;
} i2c_hid_t;

/* розбір дескриптора звіту: кількість полів або <0 */
int  hid_parse(const uint8_t *d, int len, hid_desc_t *out);
/* декодування вхідного звіту (без 2-байтової довжини). 1 — це кадр тачпада/миші, 0 — інший звіт */
int  hid_decode_input(const hid_desc_t *desc, const uint8_t *rep, int len, hid_frame_t *fr);

/* знайти пристрій. 1 — знайдено й увімкнено. */
int  i2c_hid_find(i2c_hid_t *h);

/* (повторно) перемкнути тачпад у режим Precision Touchpad і перевірити результат */
void i2c_hid_set_mode(i2c_hid_t *h);

/* НЕ блокує. 1 — прийшов новий кадр у fr. */
int  i2c_hid_poll(i2c_hid_t *h, hid_frame_t *fr);

#endif
