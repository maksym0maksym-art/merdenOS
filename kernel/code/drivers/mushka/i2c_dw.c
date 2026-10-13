#include "../../../headers/i2c_dw.h"
#include "../../../headers/PCI.h"
#include "../../../../standarts_lib/stdio.h"
#include "../../../headers/klog.h"
#undef t_printf
#define t_printf klog_rawf   /* лог іде в журнал ядра: видно в dmesg і потрапляє в savelog */

#ifndef I2C_HOST_TEST
#include "../../../headers/ports.h"
#endif

/* ---- регістри DesignWare ---- */
#define IC_CON            0x00
#define IC_TAR            0x04
#define IC_DATA_CMD       0x10
#define IC_SS_SCL_HCNT    0x14
#define IC_SS_SCL_LCNT    0x18
#define IC_FS_SCL_HCNT    0x1C
#define IC_FS_SCL_LCNT    0x20
#define IC_INTR_MASK      0x30
#define IC_RAW_INTR_STAT  0x34
#define IC_RX_TL          0x38
#define IC_TX_TL          0x3C
#define IC_CLR_INTR       0x40
#define IC_CLR_TX_ABRT    0x54
#define IC_ENABLE         0x6C
#define IC_STATUS         0x70
#define IC_TX_ABRT_SOURCE 0x80
#define IC_ENABLE_STATUS  0x9C
#define IC_COMP_PARAM_1   0xF4
#define IC_COMP_TYPE      0xFC

#define DW_COMP_TYPE_VALUE 0x44570140u

#define CON_MASTER      (1u << 0)
#define CON_SPEED_SS    (1u << 1)          /* стандартний режим, 100 кГц */
#define CON_RESTART_EN  (1u << 5)
#define CON_SLAVE_DIS   (1u << 6)

#define CMD_READ        (1u << 8)
#define CMD_STOP        (1u << 9)
#define CMD_RESTART     (1u << 10)

#define ST_ACTIVITY     (1u << 0)
#define ST_TFNF         (1u << 1)          /* у TX FIFO є місце */
#define ST_TFE          (1u << 2)          /* TX FIFO порожній */
#define ST_RFNE         (1u << 3)          /* у RX FIFO є дані */
#define ST_MST_ACTIVITY (1u << 5)

#define INTR_TX_ABRT    (1u << 6)

/* Intel LPSS: службові регістри за базою + 0x200 */
#define LPSS_PRIV_RESETS 0x204
#define LPSS_RESETS_ALL  0x7

/* ------------------------------------------------------------------ */
/*  Доступ до MMIO / час (у тесті на хості підміняється)                */
/* ------------------------------------------------------------------ */
#ifdef I2C_HOST_TEST
uint32_t host_mmio_rd(uintptr_t base, uint32_t off);
void     host_mmio_wr(uintptr_t base, uint32_t off, uint32_t v);
#define RD(c, off)     host_mmio_rd((c)->base, (off))
#define WR(c, off, v)  host_mmio_wr((c)->base, (off), (v))
#else
static inline uint32_t RD(const dw_i2c_t *c, uint32_t off) { return *(volatile uint32_t *)(c->base + off); }
static inline void     WR(const dw_i2c_t *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(c->base + off) = v; }

static uint32_t cycles_ms;

static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Частоту TSC міряємо по PIT (канал 2) за 16 мс. Без ділення: зсув на 4. */
static void calibrate(void) {
    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, 19091 & 0xFF);                    /* 1193182 Гц * 16 мс */
    outb(0x42, 19091 >> 8);
    uint64_t t0 = rdtsc_now();
    for (uint32_t i = 0; i < 4000000; i++)
        if (inb(0x61) & 0x20) break;
    uint64_t t1 = rdtsc_now();
    uint64_t d = (t1 - t0) >> 4;
    cycles_ms = (d > 100000 && d < 20000000) ? (uint32_t)d : 2000000u;   /* страховка: 2 ГГц */
}

uint64_t i2c_now(void) { return rdtsc_now(); }

uint32_t i2c_cycles_per_ms(void) {
    if (!cycles_ms) calibrate();
    return cycles_ms;
}

void i2c_delay_ms(uint32_t ms) {
    uint64_t end = rdtsc_now() + (uint64_t)ms * i2c_cycles_per_ms();
    while (rdtsc_now() < end) { }
}
#endif

/* ------------------------------------------------------------------ */
/*  Знаходження контролерів на PCI                                      */
/* ------------------------------------------------------------------ */
static void pci_power_d0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t status = pci_config_read16(bus, slot, func, 0x06);
    if (!(status & 0x10)) return;                                  /* нема списку можливостей */
    uint8_t ptr = (uint8_t)(pci_config_read32(bus, slot, func, 0x34) & 0xFC);
    for (int guard = 0; ptr >= 0x40 && guard < 32; guard++) {
        uint32_t hdr = pci_config_read32(bus, slot, func, ptr);
        if ((hdr & 0xFF) == 0x01) {                                /* Power Management */
            uint16_t pmcsr = pci_config_read16(bus, slot, func, (uint8_t)(ptr + 4));
            if (pmcsr & 0x3) {
                pci_config_write16(bus, slot, func, (uint8_t)(ptr + 4), (uint16_t)(pmcsr & ~0x3));
                i2c_delay_ms(15);                                  /* D3hot -> D0 */
            }
            return;
        }
        ptr = (uint8_t)((hdr >> 8) & 0xFC);
    }
}

#ifndef I2C_HOST_TEST
/* BIOS на цьому ноуті лишає BAR0 в I2C-контролерів нульовим (його задає ОС).
 * Тому шукаємо вільне місце в MMIO-дірі нижче 4 ГБ і пишемо адресу самі. */
static void pw32(uint8_t b, uint8_t s, uint8_t f, uint8_t off, uint32_t v) {
    outl(0xCF8, (1u << 31) | ((uint32_t)b << 16) | ((uint32_t)s << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static uint32_t mmio_top(void) {
    uint32_t top = 0;
    for (int bus = 0; bus < 8; bus++) {
        for (int sl = 0; sl < 32; sl++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; else continue; }
                uint32_t cl  = pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 8) >> 24;
                uint32_t hdr = (pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0x0C) >> 16) & 0x7F;
                if (hdr == 1) {                                   /* міст: вікно пам'яті */
                    uint32_t w = pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0x20);
                    uint32_t mb = (w & 0xFFF0u) << 16;
                    uint32_t ml = ((w >> 16) & 0xFFF0u) << 16 | 0xFFFFFu;
                    if (ml >= mb && mb >= 0x40000000u && ml < 0xE0000000u && ml + 1 > top) top = ml + 1;
                    continue;
                }
                if (hdr != 0) continue;
                uint16_t cmd = pci_config_read16((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0x04);
                for (int bo = 0x10; bo <= 0x24; bo += 4) {
                    uint32_t v = pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, (uint8_t)bo);
                    int is64 = (!(v & 1) && ((v >> 1) & 3) == 2);
                    if (v & 1) continue;
                    uint32_t base = v & ~0xFu;
                    if (is64) bo += 4;
                    if (base < 0x40000000u || base >= 0xE0000000u) continue;
                    uint32_t size;
                    if (cl == 0x03) {
                        size = 0x20000000u;                       /* відеокарта: не чіпаємо, беремо із запасом */
                    } else {
                        int bb = is64 ? bo - 4 : bo;
                        pci_config_write16((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0x04, (uint16_t)(cmd & ~7u));
                        pw32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, (uint8_t)bb, 0xFFFFFFFFu);
                        uint32_t m = pci_config_read32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, (uint8_t)bb) & ~0xFu;
                        pw32((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, (uint8_t)bb, v);
                        pci_config_write16((uint8_t)bus, (uint8_t)sl, (uint8_t)fn, 0x04, cmd);
                        size = m ? (~m + 1u) : 0x1000u;
                        if (size == 0) size = 0x1000u;
                    }
                    if (base + size > top && base + size > base) top = base + size;
                }
            }
        }
    }
    return top;
}

static uint32_t assign_bar0(uint8_t slot, uint8_t func, uint32_t oldbar) {
    static uint32_t next = 0;
    if (!next) {
        uint32_t top = mmio_top();
        if (!top) return 0;
        next = (top + 0xFFFFFu) & ~0xFFFFFu;
        if (next >= 0xDFF00000u) { next = 0; return 0; }
    }
    uint32_t a = next;
    next += 0x1000;
    uint16_t cmd = pci_config_read16(0, slot, func, 0x04);
    pci_config_write16(0, slot, func, 0x04, (uint16_t)(cmd & ~7u));
    pw32(0, slot, func, 0x10, a | (oldbar & 0xF));
    if (((oldbar >> 1) & 3) == 2) pw32(0, slot, func, 0x14, 0);
    return a;
}
#endif

#ifndef I2C_HOST_TEST
static void dbg_hex(const char *tag, uint32_t a, uint32_t b, uint32_t c) {
    static const char H[] = "0123456789abcdef";
    char buf[48]; char *p = buf;
    uint32_t v[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        for (int i = 7; i >= 0; i--) *p++ = H[(v[k] >> (i * 4)) & 0xF];
        *p++ = ' ';
    }
    *p = 0;
    t_printf("[i2c] %s %s\n", tag, buf);
}
#else
#define dbg_hex(t, a, b, c) ((void)0)
#endif

int dw_i2c_discover(dw_i2c_t *out, int max) {
    int n = 0;
    for (uint8_t slot = 0; slot < 32 && n < max; slot++) {
        for (uint8_t func = 0; func < 8 && n < max; func++) {
            uint32_t id = pci_config_read32(0, slot, func, 0x00);
            if (id == 0xFFFFFFFFu || id == 0) continue;
            if ((id & 0xFFFF) != 0x8086) continue;                 /* LPSS — це Intel */

            uint32_t cls = pci_config_read32(0, slot, func, 0x08) >> 8;
            uint32_t major = cls >> 16;
            if (major != 0x11 && major != 0x0C) continue;          /* signal processing / serial bus */

            uint32_t bar = pci_config_read32(0, slot, func, 0x10);
            if (bar & 1) continue;                                 /* порт вводу-виводу */
            if (((bar >> 1) & 3) == 2 && pci_config_read32(0, slot, func, 0x14) != 0) continue;   /* вище 4 ГБ */
            uint32_t base = bar & ~0xFu;
#ifndef I2C_HOST_TEST
            if (!base && cls == 0x0C8000u && slot != 0x1f) {
                base = assign_bar0(slot, func, bar);
                dbg_hex("assigned slot/func  BAR0", (uint32_t)(slot * 16 + func), base, 0);
            }
#endif
            if (!base) continue;

            uint16_t cmd = pci_config_read16(0, slot, func, 0x04);
            pci_config_write16(0, slot, func, 0x04, (uint16_t)(cmd | 0x0002));    /* Memory Space */
            pci_power_d0(0, slot, func);

            dw_i2c_t *c = &out[n];
            c->base = base;
            c->pci_bus = 0; c->pci_slot = slot; c->pci_func = func;
            c->pci_device = (uint16_t)(id >> 16);
            c->busy = 0;
            c->clk_khz = 0;

            if (cls != 0x0C8000u || slot == 0x1f) continue;       /* не чіпаємо чужі пристрої */
            WR(c, LPSS_PRIV_RESETS, LPSS_RESETS_ALL);              /* вивести з скидання, як Linux */
            i2c_delay_ms(1);

            dbg_hex("comp_type  resets  cmd", RD(c, IC_COMP_TYPE), RD(c, LPSS_PRIV_RESETS), pci_config_read16(0, slot, func, 0x04));
            if (RD(c, IC_COMP_TYPE) != DW_COMP_TYPE_VALUE) continue;           /* не I2C (UART, SPI, DMA) */

            uint32_t p = RD(c, IC_COMP_PARAM_1);
            c->tx_depth = (int)((p >> 16) & 0xFF) + 1;
            c->rx_depth = (int)((p >> 8) & 0xFF) + 1;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Налаштування й транзакції                                           */
/* ------------------------------------------------------------------ */
static int disable(dw_i2c_t *c) {
    WR(c, IC_ENABLE, 0);
    for (int i = 0; i < 100; i++) {
        if (!(RD(c, IC_ENABLE_STATUS) & 1)) return 1;
        i2c_delay_ms(1);
    }
    return 0;
}

int dw_i2c_setup(dw_i2c_t *c, uint32_t clk_khz) {
    c->clk_khz = clk_khz;
    c->busy = 0;
    if (!disable(c)) return DW_ERR_BUS;

    /* ~100 кГц: tHIGH ~ 4.4 мкс, tLOW ~ 5.2 мкс. Якщо реальний такт вищий за припущений,
     * шина піде швидше, але для сучасних тачпадів (вони тягнуть 400 кГц) це безпечно. */
    uint32_t hcnt = clk_khz * 44 / 10000;
    uint32_t lcnt = clk_khz * 52 / 10000;
    WR(c, IC_SS_SCL_HCNT, hcnt);
    WR(c, IC_SS_SCL_LCNT, lcnt);
    WR(c, IC_FS_SCL_HCNT, clk_khz * 6 / 10000 + 8);
    WR(c, IC_FS_SCL_LCNT, clk_khz * 13 / 10000 + 8);
    return DW_OK;
}

static int map_abort(uint32_t src) {
    if (src & 0x3)  return DW_ERR_NOACK;          /* адреса не підтверджена (7/10-бітна) */
    if (src & 0x8)  return DW_ERR_DATA;           /* байт даних не підтверджено */
    return DW_ERR_BUS;
}

int dw_i2c_begin(dw_i2c_t *c, uint8_t addr, const uint8_t *wr, int wlen,
                 uint8_t *rd, int rlen, uint32_t timeout_ms) {
    if (wlen < 0 || rlen < 0 || (wlen == 0 && rlen == 0)) return DW_ERR_ARGS;
    if (!disable(c)) return DW_ERR_BUS;

    WR(c, IC_CON, CON_MASTER | CON_SPEED_SS | CON_RESTART_EN | CON_SLAVE_DIS);
    WR(c, IC_TAR, (uint32_t)(addr & 0x7F));
    WR(c, IC_INTR_MASK, 0);
    WR(c, IC_TX_TL, 0);
    WR(c, IC_RX_TL, 0);
    (void)RD(c, IC_CLR_INTR);
    WR(c, IC_ENABLE, 1);

    c->wbuf = wr; c->wlen = wlen; c->wsent = 0;
    c->rbuf = rd; c->rlen = rlen; c->rcmds = 0; c->rrecv = 0;
    c->abrt_src = 0;
    c->deadline = i2c_now() + (uint64_t)timeout_ms * i2c_cycles_per_ms();
    c->busy = 1;
    return DW_OK;
}

/* Якщо контролер перервав передачу — забирає причину й завершує транзакцію з помилкою. */
static int check_abort(dw_i2c_t *c) {
    if (!(RD(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT)) return 0;
    c->abrt_src = RD(c, IC_TX_ABRT_SOURCE);
    (void)RD(c, IC_CLR_TX_ABRT);
    c->busy = 0;
    return map_abort(c->abrt_src);
}

int dw_i2c_step(dw_i2c_t *c) {
    if (!c->busy) return DW_ERR_ARGS;

    int ab = check_abort(c);
    if (ab) return ab;

    /* докладаємо команди, поки є місце в TX FIFO і не переповниться RX FIFO */
    for (;;) {
        if (!(RD(c, IC_STATUS) & ST_TFNF)) break;
        if (c->wsent < c->wlen) {
            uint32_t cmd = c->wbuf[c->wsent];
            if (c->wsent == c->wlen - 1 && c->rlen == 0) cmd |= CMD_STOP;
            WR(c, IC_DATA_CMD, cmd);
            c->wsent++;
        } else if (c->rcmds < c->rlen && (c->rcmds - c->rrecv) < c->rx_depth) {
            uint32_t cmd = CMD_READ;
            if (c->rcmds == 0 && c->wlen > 0) cmd |= CMD_RESTART;
            if (c->rcmds == c->rlen - 1)      cmd |= CMD_STOP;
            WR(c, IC_DATA_CMD, cmd);
            c->rcmds++;
        } else break;
    }

    /* забираємо прочитане */
    while (c->rrecv < c->rlen && (RD(c, IC_STATUS) & ST_RFNE))
        c->rbuf[c->rrecv++] = (uint8_t)(RD(c, IC_DATA_CMD) & 0xFF);

    if (c->wsent == c->wlen && c->rcmds == c->rlen && c->rrecv == c->rlen) {
        uint32_t st = RD(c, IC_STATUS);
        if ((st & ST_TFE) && !(st & ST_MST_ACTIVITY)) {            /* усе передано, шина вільна */
            /* Перерваний обмін теж лишає TX FIFO порожнім, а шину вільною, тож без повторної
             * перевірки NACK, що прийшов щойно, виглядав би як успіх. */
            ab = check_abort(c);
            if (ab) return ab;
            c->busy = 0;
            return 1;
        }
    }

    if (i2c_now() > c->deadline) {
        c->busy = 0;
        return DW_ERR_TIMEOUT;
    }
    return 0;
}

int dw_i2c_xfer(dw_i2c_t *c, uint8_t addr, const uint8_t *wr, int wlen,
                uint8_t *rd, int rlen, uint32_t timeout_ms) {
    int r = dw_i2c_begin(c, addr, wr, wlen, rd, rlen, timeout_ms);
    if (r < 0) return r;
    for (;;) {
        r = dw_i2c_step(c);
        if (r == 1) return DW_OK;
        if (r < 0) return r;
    }
}
