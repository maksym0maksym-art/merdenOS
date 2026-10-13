/* Host-тест жестів тачпада (touchpad.c): прокрутка двома пальцями, перетягування "кнопка + другий палець",
 * тап-перетягування, переповнення черги. Кадри подаються напряму в tp_feed(). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "touchpad.h"

/* заглушки платформи */
void t_printf(const char *f, ...) { (void)f; }
int i2c_hid_find(i2c_hid_t *h) { (void)h; return 0; }
int i2c_hid_poll(i2c_hid_t *h, hid_frame_t *fr) { (void)h; (void)fr; return 0; }
uint32_t i2c_cycles_per_ms(void) { return 1000; }
uint64_t i2c_now(void) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t now;                               /* "такти": 1000 = 1 мс */
static void ms(int n) { now += (uint64_t)n * 1000; tp_tick(now); }

typedef struct { long dx, dy, wheel; uint8_t b; int ev; int pressed_left, released_left, maxb; } acc_t;
static acc_t A;
static void drain(void) {
    int dx, dy, w; uint8_t b;
    while (tp_pop(&dx, &dy, &w, &b)) {
        if ((b & 1) && !(A.b & 1)) A.pressed_left++;
        if (!(b & 1) && (A.b & 1)) A.released_left++;
        A.dx += dx; A.dy += dy; A.wheel += w; A.b = b; A.ev++;
    }
}
static void clear(void) { memset(&A, 0, sizeof A); }

static hid_frame_t fr1(int x, int y, int btn) {
    hid_frame_t f; memset(&f, 0, sizeof f); f.kind = HID_KIND_TOUCHPAD; f.n = 1; f.c[0].id = 0; f.c[0].x = x; f.c[0].y = y; f.button = btn; return f;
}
static hid_frame_t fr2(int x0, int y0, int x1, int y1, int btn) {
    hid_frame_t f; memset(&f, 0, sizeof f); f.kind = HID_KIND_TOUCHPAD; f.n = 2;
    f.c[0].id = 0; f.c[0].x = x0; f.c[0].y = y0; f.c[1].id = 1; f.c[1].x = x1; f.c[1].y = y1; f.button = btn; return f;
}
static hid_frame_t fr0(int btn) { hid_frame_t f; memset(&f, 0, sizeof f); f.kind = HID_KIND_TOUCHPAD; f.n = 0; f.button = btn; return f; }
static void feed(hid_frame_t f) { tp_feed(&f, now); drain(); ms(8); }

int main(void) {
    tp_reset(HID_KIND_TOUCHPAD, 0, 3000, 0, 1500);
    tp_set_screen(1366, 768);

    /* 1. прокрутка двома пальцями вниз/вгору */
    clear();
    for (int i = 0; i < 20; i++) feed(fr2(1000, 300 + i * 40, 1200, 300 + i * 40, 0));
    feed(fr0(0)); ms(50);
    CHECK(A.wheel > 3, "scroll down wheel=%ld", A.wheel);
    CHECK(A.dx == 0 && A.dy == 0, "scroll moved pointer dx=%ld dy=%ld", A.dx, A.dy);
    clear();
    for (int i = 0; i < 20; i++) feed(fr2(1000, 1000 - i * 40, 1200, 1000 - i * 40, 0));
    feed(fr0(0)); ms(50);
    CHECK(A.wheel < -3, "scroll up wheel=%ld", A.wheel);

    /* 2. фізичний клік пальцем 1 і перетягування ДРУГИМ пальцем (раніше це ставало прокруткою) */
    clear(); ms(500);
    feed(fr1(1000, 500, 0));
    feed(fr1(1000, 500, 1));                                   /* натиснули панель */
    CHECK(A.pressed_left == 1, "left press not seen");
    feed(fr2(1000, 500, 1600, 600, 1));                        /* опустили другий палець */
    for (int i = 1; i <= 25; i++) feed(fr2(1000, 500, 1600 + i * 30, 600 + i * 10, 1));
    CHECK(A.dx > 300, "hold+second finger dx=%ld (pointer must move)", A.dx);
    CHECK(A.wheel == 0, "hold-drag produced wheel %ld", A.wheel);
    CHECK(A.b & 1, "left button released during hold");
    feed(fr1(1000, 500, 1));
    feed(fr1(1000, 500, 0));
    CHECK(A.released_left == 1, "left release not seen");
    feed(fr0(0)); ms(500);

    /* 3. рух вказівника тим самим пальцем при натиснутій кнопці */
    clear();
    feed(fr1(500, 500, 0)); feed(fr1(500, 500, 1));
    for (int i = 1; i <= 20; i++) feed(fr1(500 + i * 40, 500, 1));
    CHECK(A.dx > 300 && (A.b & 1), "one-finger hold drag dx=%ld b=%d", A.dx, A.b);
    feed(fr1(1300, 500, 0)); feed(fr0(0)); ms(500);

    /* 4. тап і перетягування: другий дотик через 200 мс після відпускання (було 150 мс) */
    clear();
    feed(fr1(1000, 700, 0)); ms(40); feed(fr0(0));             /* тап */
    CHECK(A.pressed_left == 1, "tap did not click");
    ms(180);                                                  /* ~190 мс після відпускання */
    feed(fr1(1000, 700, 0));
    for (int i = 1; i <= 20; i++) feed(fr1(1000 + i * 40, 700, 0));
    CHECK(A.dx > 300 && (A.b & 1), "tap-drag dx=%ld b=%d", A.dx, A.b);
    feed(fr0(0));
    CHECK(!(A.b & 1) && A.released_left == 1, "tap-drag not released");
    ms(600);

    /* 5. переповнення черги не губить стан кнопки і прокрутку */
    clear(); ms(600);
    {
        hid_frame_t f;
        f = fr1(500, 500, 0); tp_feed(&f, now);
        for (int i = 0; i < 200; i++) { f = fr1(500 + i * 5, 500, 0); tp_feed(&f, now); }
        f = fr1(1500, 500, 1); tp_feed(&f, now);               /* клік у переповненій черзі */
        drain();
        CHECK(A.b & 1, "button state lost on queue overflow");
        CHECK(A.dx > 200, "motion lost on overflow dx=%ld", A.dx);
    }

    printf("tp test: %d failure(s)\n", fails);
    return fails != 0;
}
