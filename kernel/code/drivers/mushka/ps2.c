#include "../../../headers/mouse.h"
#include "../../../headers/ports.h"

/*
 * PS/2-мишка через другий канал контролера 8042.
 * Працюємо полінгом (як і клавіатура в цьому ядрі), IRQ12 вимкнено —
 * у IDT все одно немає воріт для IRQ.
 *
 * Клавіатура й мишка ділять ОДИН вихідний буфер 0x60. Біт 5 регістра статусу (0x64)
 * каже, чий це байт. keyboard_handler() віддає мишачі байти сюди (ps2_mouse_feed),
 * а ми ніколи не чіпаємо байти клавіатури.
 */

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OBF 0x01
#define ST_IBF 0x02
#define ST_AUX 0x20

#define RING 32

typedef struct { int16_t dx, dy; int8_t wheel; uint8_t buttons; } ps2_pkt_t;

static ps2_pkt_t ring[RING];
static uint8_t   ring_head, ring_tail;

static uint8_t pkt[4];
static uint8_t pkt_idx;
static int     ready;
static int     has_wheel;

static int wait_write(void) {
    for (uint32_t i = 0; i < 200000; i++)
        if (!(inb(PS2_STATUS) & ST_IBF)) return 1;
    return 0;
}

static int wait_read(uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++)
        if (inb(PS2_STATUS) & ST_OBF) return 1;
    return 0;
}

static void flush_output(void) {
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OBF); i++) (void)inb(PS2_DATA);
}

static int aux_write(uint8_t v) {
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0xD4);                 /* наступний байт — для мишки */
    if (!wait_write()) return 0;
    outb(PS2_DATA, v);
    return 1;
}

static int read_byte(uint8_t *out, uint32_t loops) {
    if (!wait_read(loops)) return 0;
    *out = inb(PS2_DATA);
    return 1;
}

/* команда мишці + очікуємо ACK (0xFA); 0xFE = "повтори" */
static int aux_cmd(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t r;
        if (!aux_write(cmd)) return 0;
        if (!read_byte(&r, 300000)) return 0;
        if (r == 0xFA) return 1;
        if (r != 0xFE) return 0;
    }
    return 0;
}

static int set_rate(uint8_t rate) {
    return aux_cmd(0xF3) && aux_cmd(rate);
}

int ps2_mouse_init(void) {
    ready = 0;
    has_wheel = 0;
    pkt_idx = 0;
    ring_head = ring_tail = 0;

    flush_output();

    if (!wait_write()) return 0;
    outb(PS2_CMD, 0xA8);                          /* увімкнути другий порт */

    /* config byte: біт1 — IRQ12 (вимикаємо), біт5 — clock мишки (має бути 0) */
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0x20);
    uint8_t cfg;
    if (!read_byte(&cfg, 300000)) return 0;
    cfg = (uint8_t)(cfg & ~0x22);
    if (!wait_write()) return 0;
    outb(PS2_CMD, 0x60);
    if (!wait_write()) return 0;
    outb(PS2_DATA, cfg);

    flush_output();

    /* reset: FA, потім AA (self-test), потім id 00 */
    if (!aux_cmd(0xFF)) return 0;                 /* нема мишки -> нема ACK */
    uint8_t r;
    if (!read_byte(&r, 3000000) || r != 0xAA) return 0;
    (void)read_byte(&r, 300000);                  /* device id, 0x00 */

    if (!aux_cmd(0xF6)) return 0;                 /* defaults */

    /* магічна послідовність IntelliMouse (колесо): 200, 100, 80, потім GET ID */
    if (set_rate(200) && set_rate(100) && set_rate(80) && aux_cmd(0xF2)) {
        if (read_byte(&r, 300000) && r == 0x03) has_wheel = 1;
    }

    (void)set_rate(100);
    if (!aux_cmd(0xF4)) return 0;                 /* почати надсилати дані */

    flush_output();
    ready = 1;
    return 1;
}

static void push(int dx, int dy, int wheel, uint8_t buttons) {
    uint8_t next = (uint8_t)((ring_head + 1) % RING);
    if (next == ring_tail) return;                /* переповнення — губимо новий пакет */
    ring[ring_head].dx = (int16_t)dx;
    ring[ring_head].dy = (int16_t)dy;
    ring[ring_head].wheel = (int8_t)wheel;
    ring[ring_head].buttons = buttons;
    ring_head = next;
}

void ps2_mouse_feed(uint8_t b) {
    if (!ready) return;

    if (pkt_idx == 0 && !(b & 0x08)) return;      /* перший байт завжди має біт 3 — ресинхронізація */

    pkt[pkt_idx++] = b;
    if (pkt_idx < (has_wheel ? 4 : 3)) return;
    pkt_idx = 0;

    uint8_t f = pkt[0];
    if (f & 0xC0) return;                         /* overflow — пакет сміттєвий */

    int dx = pkt[1] - ((f << 4) & 0x100);         /* 9-бітове зі знаком */
    int dy = pkt[2] - ((f << 3) & 0x100);
    int wheel = has_wheel ? -(int8_t)pkt[3] : 0;  /* PS/2: + це "вниз", нам треба + = "вгору" */

    push(dx, -dy, wheel, (uint8_t)(f & 0x07));    /* Y у PS/2 росте вгору, на екрані — вниз */
}

void ps2_mouse_pump(void) {
    if (!ready) return;
    for (int i = 0; i < 64; i++) {
        uint8_t st = inb(PS2_STATUS);
        if ((st & (ST_OBF | ST_AUX)) != (ST_OBF | ST_AUX)) break;   /* порожньо або це байт клавіатури */
        ps2_mouse_feed(inb(PS2_DATA));
    }
}

int ps2_mouse_read(int *dx, int *dy, int *wheel, uint8_t *buttons) {
    if (ring_tail == ring_head) return 0;
    *dx = ring[ring_tail].dx;
    *dy = ring[ring_tail].dy;
    *wheel = ring[ring_tail].wheel;
    *buttons = ring[ring_tail].buttons;
    ring_tail = (uint8_t)((ring_tail + 1) % RING);
    return 1;
}
