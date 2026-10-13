#include <stdarg.h>
#include <stdint.h>
#include "../../headers/klog.h"
#include "../../headers/i2c_dw.h"
#include "../../../system/shell/graphic/console.h"

#define KLOG_SIZE   16384
#define LINE_MAX    200

#define C_TIME  0x909090
#define C_TEXT  0xFFFFFF
#define C_OK    0x00FF00
#define C_WARN  0xFFD000
#define C_FAIL  0xFF4040

static uint8_t  ring[KLOG_SIZE];
static uint32_t r_head;           
static uint32_t r_tail;            
static uint32_t r_used;
static int      r_lines;

static int      screen_on;
static uint64_t t_zero;
static uint32_t cpms;
static int      inited;

static char     pend[LINE_MAX];
static int      pend_len;

/* 64/32 без libgcc */
static uint64_t udiv64(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = (uint32_t)r;
    return q;
}

static uint64_t elapsed_us(void)
{
    if (!inited) return 0;
    uint64_t t = i2c_now() - t_zero;
    uint32_t per_ms = cpms ? cpms : 2000000u;
    uint64_t ms = udiv64(t, per_ms, 0);
    uint32_t rem;
    udiv64(t, per_ms, &rem);                      /* залишок тактів усередині мілісекунди */
    uint32_t us = (uint32_t)udiv64((uint64_t)rem * 1000u, per_ms, 0);
    return ms * 1000u + us;
}

uint32_t klog_uptime_ms(void)
{
    return (uint32_t)udiv64(elapsed_us(), 1000, 0);
}

/* ---- мінімальний форматер ---- */
static char *put_num(char *d, char *end, uint32_t v, uint32_t base, int width, char pad, int upper)
{
    char t[12];
    int n = 0;
    const char *H = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) t[n++] = '0';
    while (v && n < 11) { t[n++] = H[v % base]; v /= base; }
    while (n < width && n < 11) t[n++] = pad;
    while (n && d < end) *d++ = t[--n];
    return d;
}

static void vfmt(char *buf, int cap, const char *f, va_list ap)
{
    char *d = buf, *end = buf + cap - 1;
    while (*f && d < end) {
        if (*f != '%') { *d++ = *f++; continue; }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        switch (*f) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { *d++ = '-'; d = put_num(d, end, (uint32_t)0 - (uint32_t)v, 10, width, pad, 0); }
            else d = put_num(d, end, (uint32_t)v, 10, width, pad, 0);
            break;
        }
        case 'u': d = put_num(d, end, va_arg(ap, uint32_t), 10, width, pad, 0); break;
        case 'x': d = put_num(d, end, va_arg(ap, uint32_t), 16, width, pad, 0); break;
        case 'X': d = put_num(d, end, va_arg(ap, uint32_t), 16, width, pad, 1); break;
        case 'c': *d++ = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && d < end) {
                char c = *s++;
                *d++ = ((unsigned char)c >= 0x80) ? '?' : c;
            }
            break;
        }
        case '%': *d++ = '%'; break;
        case 0: f--; break;
        default: *d++ = '%'; if (d < end) *d++ = *f; break;
        }
        if (*f) f++;
    }
    *d = 0;
}

/* ---- кільцевий буфер ---- */
static void drop_oldest(void)
{
    while (r_used && ring[r_tail] != '\n') { r_tail = (r_tail + 1) % KLOG_SIZE; r_used--; }
    if (r_used) { r_tail = (r_tail + 1) % KLOG_SIZE; r_used--; }
    if (r_lines) r_lines--;
}

static void ring_put(uint8_t b) { ring[r_head] = b; r_head = (r_head + 1) % KLOG_SIZE; r_used++; }

static void ring_line(int level, const char *text)
{
    uint32_t need = 2;
    for (const char *p = text; *p; p++) need++;
    if (need > KLOG_SIZE / 2) return;
    while (KLOG_SIZE - r_used < need) drop_oldest();
    ring_put((uint8_t)level);
    for (const char *p = text; *p; p++) ring_put((uint8_t)*p);
    ring_put('\n');
    r_lines++;
}

/* ---- екран ---- */
static void put_timestamp(char *out)   /* "[   12.345678] " */
{
    uint64_t us = elapsed_us();
    uint32_t frac;
    uint64_t s = udiv64(us, 1000000u, &frac);
    char *d = out;
    *d++ = '[';
    d = put_num(d, out + 24, (uint32_t)s, 10, 5, ' ', 0);
    *d++ = '.';
    d = put_num(d, out + 24, frac, 10, 6, '0', 0);
    *d++ = ']';
    *d++ = ' ';
    *d = 0;
}

static const char *tag_text(int level)
{
    switch (level) {
    case KLOG_OK:   return "[  OK  ] ";
    case KLOG_WARN: return "[ WARN ] ";
    case KLOG_FAIL: return "[FAILED] ";
    default:        return "";
    }
}

static uint32_t tag_color(int level)
{
    switch (level) {
    case KLOG_OK:   return C_OK;
    case KLOG_WARN: return C_WARN;
    case KLOG_FAIL: return C_FAIL;
    default:        return C_TEXT;
    }
}

static void emit(int level, const char *msg)
{
    char ts[32];
    put_timestamp(ts);

    char line[LINE_MAX + 48];
    char *d = line;
    for (const char *p = ts; *p; p++) *d++ = *p;
    for (const char *p = tag_text(level); *p; p++) *d++ = *p;
    for (const char *p = msg; *p && d < line + sizeof line - 1; p++) *d++ = *p;
    *d = 0;

    ring_line(level, line);

    if (screen_on) {
        console_puts_color(ts, C_TIME);
        if (level != KLOG_INFO) console_puts_color(tag_text(level), tag_color(level));
        console_puts_color(msg, C_TEXT);
        console_puts_color("\n", C_TEXT);
    }
}

/* ---- публічне ---- */
void klog_init(void)
{
    r_head = r_tail = r_used = 0;
    r_lines = 0;
    pend_len = 0;
    cpms = i2c_cycles_per_ms();        /* калібрування займає ~16 мс */
    t_zero = i2c_now();
    inited = 1;
}

void klog_screen(int on) { screen_on = on; }
int  klog_booting(void)  { return screen_on; }

#define DEF(name, lvl) \
void name(const char *fmt, ...) { \
    char b[LINE_MAX]; va_list ap; va_start(ap, fmt); vfmt(b, sizeof b, fmt, ap); va_end(ap); \
    emit(lvl, b); }

DEF(klog,      KLOG_INFO)
DEF(klog_ok,   KLOG_OK)
DEF(klog_warn, KLOG_WARN)
DEF(klog_fail, KLOG_FAIL)

void klog_vraw(const char *fmt, va_list ap)
{
    char b[LINE_MAX];
    vfmt(b, sizeof b, fmt, ap);
    klog_raw(b);
}

void klog_rawf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_vraw(fmt, ap);
    va_end(ap);
}

void klog_raw(const char *t)
{
    if (!t) return;
    for (; *t; t++) {
        if (*t == '\n') {
            pend[pend_len] = 0;
            if (pend_len) emit(KLOG_INFO, pend);
            pend_len = 0;
        } else if (*t == '\r') {
            /* ігноруємо */
        } else if (pend_len < LINE_MAX - 1) {
            pend[pend_len++] = *t;
        }
    }
}

int klog_lines(void) { return r_lines; }

int klog_dump(klog_sink_fn sink)
{
    char line[LINE_MAX + 48];
    int n = 0;
    uint32_t p = r_tail;
    for (uint32_t left = r_used; left; ) {
        int level = ring[p];
        p = (p + 1) % KLOG_SIZE; left--;
        int len = 0;
        while (left && ring[p] != '\n') {
            if (len < (int)sizeof line - 1) line[len++] = (char)ring[p];
            p = (p + 1) % KLOG_SIZE; left--;
        }
        if (left) { p = (p + 1) % KLOG_SIZE; left--; }   /* '\n' */
        line[len] = 0;
        sink(level, line);
        n++;
    }
    return n;
}

static void dmesg_line(int level, const char *line)
{
    /* "[sssss.uuuuuu] " далі тег (9 символів) або текст */
    char head[24];
    int i = 0;
    while (line[i] && i < (int)sizeof head - 1) {
        head[i] = line[i]; i++;
        if (head[i - 1] == ' ' && i >= 2 && head[i - 2] == ']') break;   /* кінець "[...] " */
    }
    head[i] = 0;
    console_puts_color(head, C_TIME);
    const char *rest = line + i;
    if (level != KLOG_INFO) {
        char tag[16];
        for (int k = 0; k < 9 && rest[k]; k++) tag[k] = rest[k];
        tag[9] = 0;
        console_puts_color(tag, tag_color(level));
        rest += 9;
    }
    console_puts_color(rest, C_TEXT);
    console_puts_color("\n", C_TEXT);
}

void klog_dmesg(void) { klog_dump(dmesg_line); }
