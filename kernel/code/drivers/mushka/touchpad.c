#include "../../../headers/touchpad.h"
#include "../../../../standarts_lib/stdio.h"
#include "../../../headers/klog.h"
#undef t_printf
#define t_printf klog_rawf   /* лог іде в журнал ядра: видно в dmesg і потрапляє в savelog */

#define TAP_MS       250       /* дотик коротший за це — тап */
#define TAP_HOLD_MS  250       /* скільки тримати кнопку після тапа (вікно для перетягування) */
#define TAP_HOLD_R   50

/* ---- чутливість (можна міняти на льоту: tp_set_speed / команда `tpspeed`) ----
 * TP_SPEED_PCT: яку частину ширини екрана проходить курсор, коли палець перетинає ВЕСЬ тачпад повільним рухом.
 * Прискорення додає до цього при швидких рухах. Мертва зона гасить тремтіння пальця під час тапа:
 * курсор не рушає, поки палець не змістився більше ніж на ширину_тачпада / TP_DEADZONE_DIV. */
#define TP_SPEED_PCT     40
#define TP_DEADZONE_DIV  40
#define TP_GAP_MS        100       /* пауза між кадрами довша за це: попередня позиція пальця вже застаріла */
#define TP_JUMP_DIV      6         /* зсув за один кадр більший за ширину/6 — це стрибок (втрачені кадри), а не рух */
#define TP_ACC_MIN       150       /* множник на повільних рухах (256 = 1.0x) */
#define TP_ACC_GAIN      5         /* наскільки швидко росте прискорення */
#define TP_ACC_MAXADD    400       /* і до якої межі (разом ~2.2x на швидких рухах) */

#define QN 64                  /* черга подій (степінь двійки) */

typedef struct { int dx, dy, wheel; uint8_t b; } ev_t;

static struct {
    int kind;
    int xmin, xmax, ymin, ymax, rangex, rangey;
    int screen_w, screen_h;
    uint32_t base_q8;

    ev_t q[QN]; int qh, qt;
    uint8_t btn;               /* кнопки, які тримаються зараз (фізична + тап) */
    uint8_t phys_btn;

    int prev_n;
    int tracking_id, have_last, lx, ly;
    int rem_x, rem_y;

    uint64_t t_down;
    int moved, max_fingers, scrolled, clicked;
    int tap_pending;           /* кнопка від тапа ще тримається */
    uint64_t release_at;
    uint8_t tap_btn;
    int dragging;

    int scroll_acc, have_avg, lavg;
    int dead, slop;            /* мертва зона: поки палець майже не зсунувся, курсор стоїть */
    uint64_t last_t;           /* час останнього кадру */

    /* перетягування "кнопка + другий палець": попередні позиції пальців за id */
    int pn;
    struct { int id, x, y; } pp[6];
    int dbg_fingers;
    int sw_have, sw_lx, sw_ly, sw_ax, sw_ay;
    int swipe;
} T;


static i2c_hid_t dev;
static int speed_pct = TP_SPEED_PCT;

static uint64_t ms2c(uint32_t ms) { return (uint64_t)ms * i2c_cycles_per_ms(); }
static int iabs(int v) { return v < 0 ? -v : v; }

static void recalc(void) {
    int rx = T.rangex > 0 ? T.rangex : 1;
    /* повна ширина тачпада (повільним рухом) = speed_pct % ширини екрана */
    T.base_q8 = (uint32_t)(T.screen_w * speed_pct * 256 / 100) / (uint32_t)rx;
    if (T.base_q8 < 8) T.base_q8 = 8;
}

void tp_reset(int kind, int xmin, int xmax, int ymin, int ymax) {
    int sw = T.screen_w ? T.screen_w : 1024, sh = T.screen_h ? T.screen_h : 768;
    T.kind = kind;
    T.xmin = xmin; T.xmax = xmax; T.ymin = ymin; T.ymax = ymax;
    T.rangex = xmax - xmin; T.rangey = ymax - ymin;
    if (T.rangex < 1) T.rangex = 1;
    if (T.rangey < 1) T.rangey = 1;
    T.screen_w = sw; T.screen_h = sh;
    T.qh = T.qt = 0;
    T.btn = T.phys_btn = 0;
    T.prev_n = 0; T.tracking_id = -1; T.have_last = 0;
    T.rem_x = T.rem_y = 0;
    T.moved = T.max_fingers = T.scrolled = T.clicked = 0;
    T.tap_pending = 0; T.dragging = 0;
    T.scroll_acc = 0; T.have_avg = 0; T.pn = 0; T.dbg_fingers = 0;
    T.dead = 0; T.slop = 0; T.last_t = 0;
    T.sw_have = 0; T.sw_ax = T.sw_ay = 0; T.swipe = 0;
    recalc();
}

void tp_set_speed(int pct) {
    if (pct < 5) pct = 5;
    if (pct > 200) pct = 200;
    speed_pct = pct;
    recalc();
}

int tp_get_speed(void) { return speed_pct; }

void tp_set_screen(int w, int h) {
    if (w > 0) T.screen_w = w;
    if (h > 0) T.screen_h = h;
    recalc();
}

int tp_kind(void) { return T.kind; }
int tp_fingers(void) { return T.dbg_fingers; }
void tp_mode_info(int *verified, int *ok, int *sent, uint32_t *n_touch, uint32_t *n_mouse) {
    *verified = dev.mode_verified; *ok = dev.mode_ok; *sent = dev.mode_sent;
    *n_touch = dev.n_touch; *n_mouse = dev.n_mouse;
}

/* ---- черга подій ---- */
static void emit(int dx, int dy, int wheel) {
    int next = (T.qt + 1) & (QN - 1);
    if (next == T.qh) {                                /* черга повна: не губимо, а зливаємо з останньою подією */
        ev_t *l = &T.q[(T.qt + QN - 1) & (QN - 1)];
        l->dx += dx; l->dy += dy; l->wheel += wheel; l->b = T.btn;
        return;
    }
    T.q[T.qt].dx = dx; T.q[T.qt].dy = dy; T.q[T.qt].wheel = wheel; T.q[T.qt].b = T.btn;
    T.qt = next;
}

int tp_pop(int *dx, int *dy, int *wheel, uint8_t *buttons) {
    if (T.qh == T.qt) return 0;
    *dx = T.q[T.qh].dx; *dy = T.q[T.qh].dy; *wheel = T.q[T.qh].wheel; *buttons = T.q[T.qh].b;
    T.qh = (T.qh + 1) & (QN - 1);
    return 1;
}

void tp_tick(uint64_t now) {
    if (T.tap_pending && now >= T.release_at) {
        T.btn &= (uint8_t)~T.tap_btn;
        T.tap_pending = 0;
        emit(0, 0, 0);
    }
}

/* ---- жести ---- */
static void on_touch(uint64_t now, int n) {
    T.t_down = now;
    T.moved = 0; T.max_fingers = n; T.scrolled = 0; T.clicked = 0;
    T.rem_x = T.rem_y = 0;
    T.tracking_id = -1;
    T.dead = 1; T.slop = 0;                                             /* новий дотик: спершу мертва зона */
    if (T.tap_pending && now < T.release_at && T.tap_btn == 1) {       /* тап і одразу палець: перетягування */
        T.dragging = 1;
        T.tap_pending = 0;                                              /* кнопка лишається натиснутою */
    }
}

static void tap(uint8_t b, uint32_t hold_ms, uint64_t now) {
    T.btn |= b;
    emit(0, 0, 0);
    T.tap_pending = 1;
    T.tap_btn = b;
    T.release_at = now + ms2c(hold_ms);
}

static void on_lift(uint64_t now) {
    int quick = (now - T.t_down) < ms2c(TAP_MS);
    int still = T.moved < T.rangex / 25 + 1;

    if (T.dragging) {
        T.dragging = 0;
        T.btn &= (uint8_t)~1;
        emit(0, 0, 0);
        if (quick && still) tap(1, TAP_HOLD_MS, now);                  /* подвійний тап = подвійний клік */
        return;
    }
    if (T.clicked || T.scrolled || !quick || !still) return;

    if (T.max_fingers >= 3)       tap(4, TAP_HOLD_R, now);
    else if (T.max_fingers == 2)  tap(2, TAP_HOLD_R, now);
    else                          tap(1, TAP_HOLD_MS, now);
}

/* зсув вказівника з прискоренням */
static void move_by(int dx, int dy) {
    if (iabs(dx) > T.rangex / 3 || iabs(dy) > T.rangey / 3) return;     /* стрибок датчика */

    T.moved += iabs(dx) + iabs(dy);
    if (!dx && !dy) return;

    if (T.dead) {                                                       /* тремтіння пальця при тапі не рухає курсор */
        if (T.dragging || T.phys_btn) T.dead = 0;                       /* перетягування — одразу без затримки */
        else {
            T.slop += iabs(dx) + iabs(dy);
            if (T.slop < T.rangex / TP_DEADZONE_DIV + 1) return;
            T.dead = 0;
        }
    }

    int norm = (iabs(dx) + iabs(dy)) * 1000 / T.rangex;                 /* швидкість: проміле ширини за звіт */
    int acc = TP_ACC_MIN + (norm * TP_ACC_GAIN > TP_ACC_MAXADD ? TP_ACC_MAXADD : norm * TP_ACC_GAIN);
    int32_t gain = (int32_t)((T.base_q8 * (uint32_t)acc) >> 8);

    int32_t tx = T.rem_x + dx * gain, ty = T.rem_y + dy * gain;
    int ox = tx >> 8, oy = ty >> 8;
    T.rem_x = tx - ox * 256;
    T.rem_y = ty - oy * 256;
    if (ox || oy) emit(ox, oy, 0);
}

static void do_move(const hid_frame_t *f) {
    const hid_contact_t *c = &f->c[0];
    for (int i = 0; i < f->n; i++) if (f->c[i].id == T.tracking_id) { c = &f->c[i]; break; }
    if (c->id != T.tracking_id) { T.tracking_id = c->id; T.have_last = 0; }

    if (!T.have_last) { T.lx = c->x; T.ly = c->y; T.have_last = 1; return; }

    int dx = c->x - T.lx, dy = c->y - T.ly;
    T.lx = c->x; T.ly = c->y;
    /* Стрибок: палець не може проїхати пів тачпада за один кадр. Так виглядають втрачені кадри або новий дотик
     * без кадру відпускання — курсор не має стрибати на різницю позицій. */
    if (iabs(dx) > T.rangex / TP_JUMP_DIV || iabs(dy) > T.rangey / TP_JUMP_DIV) return;
    move_by(dx, dy);
}

/* Кнопка натиснута (або тримається тап-перетягування), а рухається ДРУГИЙ палець: це перетягування,
 * а не прокрутка. Рухаємо вказівник тим пальцем, що змістився найбільше. */
static void do_move_multi(const hid_frame_t *f) {
    int bdx = 0, bdy = 0, best = -1, np = 0;
    struct { int id, x, y; } nw[6];
    for (int i = 0; i < f->n && np < 6; i++) {
        const hid_contact_t *c = &f->c[i];
        int k;
        for (k = 0; k < T.pn; k++) if (T.pp[k].id == c->id) break;
        if (k < T.pn) {
            int dx = c->x - T.pp[k].x, dy = c->y - T.pp[k].y;
            int m = iabs(dx) + iabs(dy);
            if (iabs(dx) <= T.rangex / 3 && iabs(dy) <= T.rangey / 3 && m > best) { best = m; bdx = dx; bdy = dy; }
        }
        nw[np].id = c->id; nw[np].x = c->x; nw[np].y = c->y; np++;
    }
    for (int i = 0; i < np; i++) { T.pp[i].id = nw[i].id; T.pp[i].x = nw[i].x; T.pp[i].y = nw[i].y; }
    T.pn = np;
    if (best >= 0) move_by(bdx, bdy);
}

static void do_scroll(const hid_frame_t *f) {
    int ay = (f->c[0].y + f->c[1].y) / 2;
    if (!T.have_avg) { T.lavg = ay; T.have_avg = 1; return; }
    int d = ay - T.lavg;
    T.lavg = ay;
    T.moved += iabs(d);
    if (T.moved > T.rangex / 25 + 1) T.scrolled = 1;

    int step = T.rangey / 16;
    if (step < 8) step = 8;
    T.scroll_acc += d;
    while (T.scroll_acc >= step)  { T.scroll_acc -= step; T.scrolled = 1; emit(0, 0, +1); }
    while (T.scroll_acc <= -step) { T.scroll_acc += step; T.scrolled = 1; emit(0, 0, -1); }
}

static void phys_click(const hid_frame_t *f) {
    int pressed = f->button != 0;
    if (pressed && !T.phys_btn) {
        uint8_t b;
        if (f->button & 6) b = (uint8_t)(f->button & 6);                /* пристрій сам розрізняє кнопки */
        else if (f->n >= 3) b = 4;
        else if (f->n == 2) b = 2;
        else if (f->n == 1 && (f->c[0].x - T.xmin) * 10 > T.rangex * 7 && (f->c[0].y - T.ymin) * 10 > T.rangey * 8) b = 2;
        else b = 1;
        T.phys_btn = b;
        T.btn |= b;
        T.clicked = 1;
        if (T.tap_pending) { T.btn &= (uint8_t)~T.tap_btn; T.tap_pending = 0; }
        emit(0, 0, 0);
    } else if (!pressed && T.phys_btn) {
        T.btn &= (uint8_t)~T.phys_btn;
        T.phys_btn = 0;
        emit(0, 0, 0);
    }
}

#define TP_SWIPE_DIV 8         /* свайп спрацьовує після зсуву на 1/8 ширини тачпада */

static void do_swipe3(const hid_frame_t *f) {
    int n = f->n, ax = 0, ay = 0;
    for (int i = 0; i < n; i++) { ax += f->c[i].x; ay += f->c[i].y; }
    ax /= n; ay /= n;                                   /* центр трьох пальців */

    if (!T.sw_have) { T.sw_lx = ax; T.sw_ly = ay; T.sw_ax = T.sw_ay = 0; T.sw_have = 1; return; }

    int dx = ax - T.sw_lx, dy = ay - T.sw_ly;
    T.sw_lx = ax; T.sw_ly = ay;
    if (iabs(dx) > T.rangex / 3 || iabs(dy) > T.rangey / 3) return;   /* стрибок датчика */

    T.moved += iabs(dx) + iabs(dy);                     /* щоб кінець свайпу не став тапом (середня кнопка) */
    T.sw_ax += dx; T.sw_ay += dy;

    if (iabs(T.sw_ax) > T.rangex / TP_SWIPE_DIV && iabs(T.sw_ax) > 2 * iabs(T.sw_ay)) {
        T.swipe += T.sw_ax < 0 ? -1 : 1;
        if (T.swipe > 3) T.swipe = 3;
        if (T.swipe < -3) T.swipe = -3;
        T.scrolled = 1;
        T.sw_ax = T.sw_ay = 0;                          /* можна продовжити рух і перейти ще на один стіл */
    } else if (iabs(T.sw_ay) > T.rangey / TP_SWIPE_DIV) {
        T.sw_ax = T.sw_ay = 0;                          /* переважно вертикально: не накопичуємо дрейф */
    }
}

int tp_swipe_pop(void) {
    int s = T.swipe;
    T.swipe = 0;
    return s < 0 ? -1 : s > 0 ? 1 : 0;
}

void tp_feed(const hid_frame_t *f, uint64_t now) {
    if (f->kind == HID_KIND_MOUSE) {
        T.btn = (uint8_t)(f->button & 7);
        emit(f->dx, f->dy, f->wheel);
        return;
    }

    int n = f->n;
    phys_click(f);

    /* Довга пауза між кадрами, поки палець «був»: кадр відпускання загубився (або GUI довго малював).
     * Стара позиція застаріла — рахуємо від нової, щоб курсор не стрибав на різницю позицій. */
    if (n > 0 && T.prev_n > 0 && T.last_t && now - T.last_t > ms2c(TP_GAP_MS)) {
        T.have_last = 0; T.have_avg = 0; T.scroll_acc = 0; T.pn = 0;
    }
    T.last_t = now;

    if (n == 0) {
        if (T.prev_n > 0) on_lift(now);
        T.have_last = 0; T.have_avg = 0; T.scroll_acc = 0; T.pn = 0;
        T.sw_have = 0;
    } else {
        if (T.prev_n == 0) on_touch(now, n);
        if (n > T.max_fingers) T.max_fingers = n;
        if (n != T.prev_n) { T.have_last = 0; T.have_avg = 0; T.sw_have = 0;}
        int hold = T.phys_btn || T.dragging;
        if (n >= 2 && hold)  do_move_multi(f);
        else {
            T.pn = 0;
            if (n == 1)      do_move(f);
            else if (n == 2) do_scroll(f);
            else if (n == 3) do_swipe3(f);
        }
        T.dbg_fingers = n;
    }
    T.prev_n = n;
}

int tp_init(void) {
    if (!i2c_hid_find(&dev)) return 0;
    tp_reset(dev.kind, dev.xmin, dev.xmax, dev.ymin, dev.ymax);
    t_printf("[i2c] %s ready\n", dev.kind == HID_KIND_TOUCHPAD ? "touchpad" : "I2C mouse");
    return 1;
}

/* Звіт читається до кінця за один виклик (кілька сотень мікросекунд). Раніше кожен виклик просував
 * I2C-автомат лише на крок, тому при повільному малюванні (перетягування/зміна розміру вікна) звіти
 * тачпада читались рідко й втрачались — кнопка/жести "не реєструвались". */
static int fetch_frame(hid_frame_t *fr) {
    uint64_t dl = i2c_now() + 3 * (uint64_t)i2c_cycles_per_ms();
    for (;;) {
        if (i2c_hid_poll(&dev, fr)) return 1;
        if (!dev.reading) return 0;
        if (i2c_now() > dl) return 0;
    }
}

int tp_poll(int *dx, int *dy, int *wheel, uint8_t *buttons) {
    if (dev.kind == HID_KIND_NONE) return 0;
    uint64_t now = i2c_now();
    tp_tick(now);
    if (tp_pop(dx, dy, wheel, buttons)) return 1;

    hid_frame_t fr;
    for (int guard = 0; guard < 8; guard++) {
        if (!fetch_frame(&fr)) return 0;
        tp_feed(&fr, now);
        if (tp_pop(dx, dy, wheel, buttons)) return 1;
    }
    return 0;
}
