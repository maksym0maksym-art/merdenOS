#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/* Рівні / теги рядка */
enum {
    KLOG_INFO = 1,   /* звичайний рядок                */
    KLOG_OK   = 2,   /* [  OK  ] зелений               */
    KLOG_WARN = 3,   /* [ WARN ] жовтий                */
    KLOG_FAIL = 4    /* [FAILED] червоний              */
};

void klog_init(void);       
void klog_screen(int on);   
int  klog_booting(void);                      

void klog(const char *fmt, ...);           
void klog_ok(const char *fmt, ...);                                   
void klog_warn(const char *fmt, ...);
void klog_fail(const char *fmt, ...);

void klog_raw(const char *text);
void klog_rawf(const char *fmt, ...);        /* printf у журнал (як t_printf, але потрапляє в dmesg/savelog) */
#include <stdarg.h>
void klog_vraw(const char *fmt, va_list ap);   /* printf у klog_raw */              /* шматки тексту; рядок завершується '\n'        */

/* dmesg: віддає рядки по одному у sink (level, text без '\n'); повертає кількість */
typedef void (*klog_sink_fn)(int level, const char *line);
int  klog_dump(klog_sink_fn sink);
int  klog_lines(void);

void klog_dmesg(void);                        /* кольоровий вивід у шелл                       */
uint32_t klog_uptime_ms(void);

#endif
