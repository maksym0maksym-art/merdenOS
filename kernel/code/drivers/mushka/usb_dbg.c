#include <stdarg.h>
#include <stdint.h>
#include "../../../headers/usb_dbg.h"
#include "../../../headers/ports.h"

void shell_print(char *text);
int  klog_booting(void);
void klog_raw(const char *text);

int usb_debug = 0;

static char *put_uint(char *d, uint32_t v, uint32_t base, int width, char pad) {
    char t[12];
    int n = 0;
    static const char H[] = "0123456789abcdef";
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = H[v % base]; v /= base; }
    while (n < width && n < 11) t[n++] = pad;
    while (n) *d++ = t[--n];
    return d;
}

void usb_printf(const char *fmt, ...) {
    char buf[256];
    char *d = buf;
    char *end = buf + sizeof(buf) - 12;
    va_list ap;
    va_start(ap, fmt);

    while (*fmt && d < end) {
        if (*fmt != '%') { *d++ = *fmt++; continue; }
        fmt++;

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { *d++ = '-'; d = put_uint(d, (uint32_t)(-v), 10, width, pad); }
            else       d = put_uint(d, (uint32_t)v, 10, width, pad);
            break;
        }
        case 'u': d = put_uint(d, va_arg(ap, uint32_t), 10, width, pad); break;
        case 'x': d = put_uint(d, va_arg(ap, uint32_t), 16, width, pad); break;
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
        case 0: fmt--; break;
        default: *d++ = '%'; *d++ = *fmt; break;
        }
        if (*fmt) fmt++;
    }
    va_end(ap);
    *d = 0;
    if (klog_booting()) klog_raw(buf);
    else                shell_print(buf);
}


static uint32_t tsc_per_ms = 0;

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void usb_time_init(void) {
    if (tsc_per_ms) return;

    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, 11932 & 0xFF);
    outb(0x42, 11932 >> 8);

    uint64_t t0 = rdtsc64();
    int ok = 0;
    for (uint32_t i = 0; i < 4000000; i++) {
        if (inb(0x61) & 0x20) { ok = 1; break; }
    }
    uint64_t t1 = rdtsc64();

    uint32_t delta = (uint32_t)(t1 - t0); 
    uint32_t per_ms = delta / 10;

    if (!ok || per_ms < 100000 || per_ms > 10000000) {
        usb_printf("[usb] TSC calibration failed (delta=%u), assuming 2 GHz\n", delta);
        per_ms = 2000000;
    }
    tsc_per_ms = per_ms;
    USB_DBG("[usb] TSC = %u kHz\n", tsc_per_ms);
}

usb_deadline_t usb_deadline_ms(uint32_t ms) {
    if (!tsc_per_ms) usb_time_init();
    return rdtsc64() + (uint64_t)ms * tsc_per_ms;
}

int usb_expired(usb_deadline_t d) {
    return rdtsc64() >= d;
}

void usb_delay_ms(uint32_t ms) {
    usb_deadline_t d = usb_deadline_ms(ms);
    while (!usb_expired(d)) __asm__ volatile ("pause");
}

void usb_delay_us(uint32_t us) {
    if (!tsc_per_ms) usb_time_init();
    uint32_t ticks = (uint32_t)(((uint64_t)us * tsc_per_ms) >> 10);
    uint64_t end = rdtsc64() + ticks;
    while (rdtsc64() < end) __asm__ volatile ("pause");
}
