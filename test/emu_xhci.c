/*
 * Емулятор xHCI-контролера + USB Mass Storage (BOT/SCSI) для host-тестів драйвера.
 *
 * Пише розробник драйвера за специфікацією xHCI 1.1, тому ловить помилки ЛОГІКИ (кільця, cycle-біти,
 * контексти, послідовності команд, обробка STALL/таймаутів), але не помилки "залізних" нюансів.
 *
 * Принцип: драйвер збирається з -DXHCI_HOSTTEST, MMIO йде через emu_rd32/emu_wr32. Усі DMA-адреси —
 * реальні адреси хоста (статичні буфери при -no-pie лежать у перших 4 ГБ, як і у 32-бітному ядрі).
 * Дзвінок у doorbell обробляється синхронно: емулятор одразу викладає події в Event Ring.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "emu_xhci.h"
#include "PCI.h"
#include "usb_dbg.h"

emu_cfg_t emu;

/* ================================================================== */
/*  Заглушки ядра: printf, час                                         */
/* ================================================================== */
int usb_debug = 0;

void usb_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (emu.verbose) { printf("   | "); vprintf(fmt, ap); }
    va_end(ap);
}

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void usb_time_init(void) {}
usb_deadline_t usb_deadline_ms(uint32_t ms) { return now_ns() + (uint64_t)ms * 1000000ull; }
int usb_expired(usb_deadline_t d) { return now_ns() >= d; }
void usb_delay_ms(uint32_t ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, 0); }
void usb_delay_us(uint32_t us) { struct timespec ts = { 0, (long)us * 1000 }; nanosleep(&ts, 0); }

/* ================================================================== */
/*  PCI                                                                */
/* ================================================================== */
#define REGS_SIZE 0x4000
static uint8_t regs[REGS_SIZE] __attribute__((aligned(4096)));
static uint8_t cfg[256];

#define CAPLEN   0x40
#define OPBASE   CAPLEN
#define RTBASE   0x600
#define DBBASE   0x800
#define XECP     0x1000
#define PORTSC(n) (OPBASE + 0x400 + 0x10 * ((n) - 1))
#define NPORTS   4
#define NSLOTS   16

static uint32_t rd_cfg32(uint8_t off) { uint32_t v; memcpy(&v, cfg + (off & 0xFC), 4); return v; }
static void wr_cfg32(uint8_t off, uint32_t v) { memcpy(cfg + (off & 0xFC), &v, 4); }

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    (void)bus; (void)slot; (void)func; return rd_cfg32(offset);
}
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint16_t)(pci_config_read32(bus, slot, func, offset) >> ((offset & 2) * 8));
}
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    (void)bus; (void)slot; (void)func;
    if ((offset & 0xFC) == 0x04) value = (value & 0xFFFF) | (rd_cfg32(0x04) & 0xFFFF0000u);   /* статус RO у нас */
    if ((offset & 0xFC) == 0x74) {                                                           /* PMCSR: тільки біти 1:0 */
        value = (rd_cfg32(0x74) & ~3u) | (value & 3u);
    }
    if ((offset & 0xFC) < 0x40 && (offset & 0xFC) != 0x04) return;                           /* решта заголовка RO */
    wr_cfg32(offset, value);
}
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t old = pci_config_read32(bus, slot, func, offset);
    uint32_t sh = (offset & 2) * 8;
    pci_config_write32(bus, slot, func, offset, (old & ~(0xFFFFu << sh)) | ((uint32_t)value << sh));
}

int pci_find_class_n(uint8_t cc, uint8_t sc, uint8_t pi, int index, pci_device_t *out) {
    if (cc != 0x0C || sc != 0x03 || pi != 0x30 || index != 0) return 0;
    memset(out, 0, sizeof(*out));
    out->bus = 0; out->slot = 0x14; out->func = 0;
    out->vendor_id = 0x8086; out->device_id = 0x9DED;
    out->class_code = cc; out->subclass = sc; out->prog_if = pi;
    out->bar0 = rd_cfg32(0x10);
    out->bar1 = rd_cfg32(0x14);
    return 1;
}

/* ================================================================== */
/*  Віртуальні USB-пристрої                                            */
/* ================================================================== */
typedef struct vdev {
    int      present;
    int      type;                 /* EMU_DEV_MSC / EMU_DEV_KBD */
    int      speed;                /* 1 FS, 3 HS, 4 SS */
    uint8_t  mps0;                 /* bMaxPacketSize0 з дескриптора (для SS — експонента) */
    int      address;
    int      configured;
    uint8_t  cfg_desc[256]; int cfg_len;
    /* MSC */
    int      fd; uint32_t bsz, nblocks;
    int      write_protected, no_medium;
    int      tur_not_ready;        /* скільки разів TUR відповість NOT READY */
    /* BOT */
    uint8_t  in_data[70000]; uint32_t in_len, in_pos;
    uint8_t  csw[13]; int csw_ready;
    uint8_t  out_buf[70000]; uint32_t out_expect, out_pos; uint8_t pend_cbw[31]; int expecting_data_out;
    uint32_t tag;
    uint8_t  sense[3];             /* key, asc, ascq */
    int      stall_in_next, stall_out_next;
    int      drop_in_next;
    uint64_t sectors_written;
    uint8_t  mq[64][4]; int mq_n;   /* черга звітів миші */
} vdev_t;

static vdev_t ports[NPORTS + 1];

static void put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, v); put_le16(p + 2, v >> 16); }
static uint32_t get_le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static void build_msc_cfg(vdev_t *d) {
    uint8_t *c = d->cfg_desc; int n = 0;
    uint16_t mps = d->speed == 4 ? 1024 : d->speed == 3 ? 512 : 64;
    /* конфігурація */
    uint8_t cf[9] = { 9, 2, 0, 0, 1, 1, 0, 0x80, 50 };
    memcpy(c + n, cf, 9); n += 9;
    /* інтерфейс 0, alt 0: BOT */
    uint8_t i0[9] = { 9, 4, 0, 0, 2, 8, 6, 0x50, 0 };
    memcpy(c + n, i0, 9); n += 9;
    uint8_t ein[7] = { 7, 5, 0x81, 2, (uint8_t)mps, (uint8_t)(mps >> 8), 0 };
    memcpy(c + n, ein, 7); n += 7;
    if (d->speed == 4) { uint8_t comp[6] = { 6, 0x30, 3 /* bMaxBurst */, 0, 0, 0 }; memcpy(c + n, comp, 6); n += 6; }
    uint8_t eout[7] = { 7, 5, 0x02, 2, (uint8_t)mps, (uint8_t)(mps >> 8), 0 };
    memcpy(c + n, eout, 7); n += 7;
    if (d->speed == 4) { uint8_t comp[6] = { 6, 0x30, 3, 0, 0, 0 }; memcpy(c + n, comp, 6); n += 6; }
    if (d->speed == 4) {
        /* alt 1: UAS (proto 0x62) — драйвер має вибрати BOT alt 0 */
        uint8_t i1[9] = { 9, 4, 0, 1, 4, 8, 6, 0x62, 0 };
        memcpy(c + n, i1, 9); n += 9;
        for (int k = 0; k < 4; k++) {
            uint8_t e[7] = { 7, 5, (uint8_t)((k & 1) ? 0x80 | (k + 1) : (k + 1)), 2, 0, 4, 0 };
            memcpy(c + n, e, 7); n += 7;
            uint8_t comp[6] = { 6, 0x30, 3, 0, 0, 0 }; memcpy(c + n, comp, 6); n += 6;
        }
    }
    put_le16(c + 2, n);
    d->cfg_len = n;
}

static void build_kbd_cfg(vdev_t *d) {
    uint8_t *c = d->cfg_desc; int n = 0;
    uint8_t cf[9] = { 9, 2, 0, 0, 1, 1, 0, 0xA0, 50 };
    memcpy(c + n, cf, 9); n += 9;
    uint8_t i0[9] = { 9, 4, 0, 0, 1, 3, 1, 1, 0 };
    memcpy(c + n, i0, 9); n += 9;
    uint8_t hid[9] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0 };
    memcpy(c + n, hid, 9); n += 9;
    uint8_t e[7] = { 7, 5, 0x81, 3, 8, 0, 10 };
    memcpy(c + n, e, 7); n += 7;
    put_le16(c + 2, n);
    d->cfg_len = n;
}

static void build_mouse_cfg(vdev_t *d) {
    uint8_t *c = d->cfg_desc; int n = 0;
    uint8_t cf[9] = { 9, 2, 0, 0, 1, 1, 0, 0xA0, 50 };
    memcpy(c + n, cf, 9); n += 9;
    uint8_t i0[9] = { 9, 4, 0, 0, 1, 3, 1, 2, 0 };
    memcpy(c + n, i0, 9); n += 9;
    uint8_t hid[9] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, 52, 0 };
    memcpy(c + n, hid, 9); n += 9;
    uint8_t e[7] = { 7, 5, 0x81, 3, 4, 0, 10 };
    memcpy(c + n, e, 7); n += 7;
    put_le16(c + 2, n);
    d->cfg_len = n;
}

void emu_reset(void) {
    memset(&emu, 0, sizeof(emu));
    memset(ports, 0, sizeof(ports));
    memset(regs, 0, sizeof(regs));
    memset(cfg, 0, sizeof(cfg));
    emu.csz = 32;
}

int emu_add_msc(int port, int speed, const char *image, uint32_t bsz) {
    vdev_t *d = &ports[port];
    memset(d, 0, sizeof(*d));
    d->present = 1; d->type = EMU_DEV_MSC; d->speed = speed;
    d->mps0 = speed == 4 ? 9 : speed == 3 ? 64 : 64;
    d->fd = open(image, O_RDWR);
    if (d->fd < 0) { perror(image); return -1; }
    struct stat st; fstat(d->fd, &st);
    d->bsz = bsz; d->nblocks = (uint32_t)(st.st_size / bsz);
    build_msc_cfg(d);
    return 0;
}

void emu_add_kbd(int port, int speed, int mps0) {
    vdev_t *d = &ports[port];
    memset(d, 0, sizeof(*d));
    d->present = 1; d->type = EMU_DEV_KBD; d->speed = speed; d->mps0 = (uint8_t)mps0;
    build_kbd_cfg(d);
}

void emu_add_mouse(int port, int speed, int mps0) {
    vdev_t *d = &ports[port];
    memset(d, 0, sizeof(*d));
    d->present = 1; d->type = EMU_DEV_MOUSE; d->speed = speed; d->mps0 = (uint8_t)mps0;
    build_mouse_cfg(d);
}

void emu_msc_set(int port, int no_medium, int wp, int tur_not_ready) {
    ports[port].no_medium = no_medium; ports[port].write_protected = wp; ports[port].tur_not_ready = tur_not_ready;
}
void emu_msc_inject(int port, int stall_in, int stall_out, int drop_in) {
    ports[port].stall_in_next = stall_in; ports[port].stall_out_next = stall_out; ports[port].drop_in_next = drop_in;
}
void emu_unplug(int port) {
    ports[port].present = 0;
    /* PORTSC: CCS=0 + CSC */
    uint32_t v; memcpy(&v, regs + PORTSC(port), 4);
    v &= ~(1u | 2u); v |= (1u << 17);
    memcpy(regs + PORTSC(port), &v, 4);
}
uint64_t emu_sectors_written(int port) { return ports[port].sectors_written; }

/* ---- запити до пристрою ---- */
static void dev_descriptor(vdev_t *d, uint8_t *o) {
    memset(o, 0, 18);
    o[0] = 18; o[1] = 1;
    put_le16(o + 2, d->speed == 4 ? 0x0300 : 0x0200);
    o[7] = d->mps0;
    put_le16(o + 8, d->type == EMU_DEV_MSC ? 0x0951 : d->type == EMU_DEV_MOUSE ? 0x1BCF : 0x046D);
    put_le16(o + 10, d->type == EMU_DEV_MSC ? 0x1666 : 0xC31C);
    o[17] = 1;
}

/* ================================================================== */
/*  SCSI / BOT                                                         */
/* ================================================================== */
static void set_sense(vdev_t *d, int k, int a, int q) { d->sense[0] = (uint8_t)k; d->sense[1] = (uint8_t)a; d->sense[2] = (uint8_t)q; }

static void bot_finish(vdev_t *d, uint32_t residue, int status) {
    put_le32(d->csw, 0x53425355u);
    put_le32(d->csw + 4, d->tag);
    put_le32(d->csw + 8, residue);
    d->csw[12] = (uint8_t)status;
    d->csw_ready = 1;
}

static void run_scsi(vdev_t *d, const uint8_t *cbw, const uint8_t *data_out) {
    const uint8_t *cb = cbw + 15;
    uint32_t dlen = get_le32(cbw + 8);
    int dir_in = (cbw[12] & 0x80) != 0;
    d->in_len = d->in_pos = 0;
    d->csw_ready = 0;
    d->tag = get_le32(cbw + 4);

    switch (cb[0]) {
    case 0x00:                                           /* TEST UNIT READY */
        if (d->no_medium) { set_sense(d, 2, 0x3A, 0); bot_finish(d, dlen, 1); return; }
        if (d->tur_not_ready > 0) { d->tur_not_ready--; set_sense(d, 2, 0x04, 1); bot_finish(d, dlen, 1); return; }
        bot_finish(d, 0, 0); return;
    case 0x03: {                                         /* REQUEST SENSE */
        memset(d->in_data, 0, 18);
        d->in_data[0] = 0x70; d->in_data[2] = d->sense[0]; d->in_data[7] = 10;
        d->in_data[12] = d->sense[1]; d->in_data[13] = d->sense[2];
        d->in_len = dlen < 18 ? dlen : 18;
        set_sense(d, 0, 0, 0);
        bot_finish(d, dlen - d->in_len, 0); return;
    }
    case 0x12: {                                         /* INQUIRY */
        memset(d->in_data, 0, 36);
        d->in_data[1] = 0x80; d->in_data[3] = 2; d->in_data[4] = 31;
        memcpy(d->in_data + 8, "EMUVendr", 8);
        memcpy(d->in_data + 16, "Virtual Flash   ", 16);
        memcpy(d->in_data + 32, "1.00", 4);
        d->in_len = dlen < 36 ? dlen : 36;
        bot_finish(d, dlen - d->in_len, 0); return;
    }
    case 0x1A:                                           /* MODE SENSE(6) */
        memset(d->in_data, 0, 4);
        d->in_data[0] = 3; d->in_data[2] = d->write_protected ? 0x80 : 0;
        d->in_len = dlen < 4 ? dlen : 4;
        bot_finish(d, dlen - d->in_len, 0); return;
    case 0x25:                                           /* READ CAPACITY(10) */
        if (d->no_medium) { set_sense(d, 2, 0x3A, 0); bot_finish(d, dlen, 1); return; }
        put_be32(d->in_data, d->nblocks - 1);
        put_be32(d->in_data + 4, d->bsz);
        d->in_len = dlen < 8 ? dlen : 8;
        bot_finish(d, dlen - d->in_len, 0); return;
    case 0x28: {                                         /* READ(10) */
        uint32_t lba = get_be32(cb + 2), n = (cb[7] << 8) | cb[8];
        if ((uint64_t)lba + n > d->nblocks || (uint64_t)n * d->bsz > sizeof(d->in_data) || !dir_in) {
            set_sense(d, 5, 0x21, 0); bot_finish(d, dlen, 1); return;
        }
        if (pread(d->fd, d->in_data, (size_t)n * d->bsz, (off_t)lba * d->bsz) != (ssize_t)((size_t)n * d->bsz)) { set_sense(d, 3, 0x11, 0); bot_finish(d, dlen, 1); return; }
        d->in_len = n * d->bsz;
        bot_finish(d, dlen - d->in_len, 0); return;
    }
    case 0x2A: {                                         /* WRITE(10) */
        uint32_t lba = get_be32(cb + 2), n = (cb[7] << 8) | cb[8];
        if (d->write_protected) { set_sense(d, 7, 0x27, 0); bot_finish(d, dlen, 1); return; }
        if ((uint64_t)lba + n > d->nblocks || dir_in) { set_sense(d, 5, 0x21, 0); bot_finish(d, dlen, 1); return; }
        if (pwrite(d->fd, data_out, (size_t)n * d->bsz, (off_t)lba * d->bsz) != (ssize_t)((size_t)n * d->bsz)) { set_sense(d, 3, 0x0C, 0); bot_finish(d, dlen, 1); return; }
        d->sectors_written += n;
        bot_finish(d, 0, 0); return;
    }
    case 0x35: fsync(d->fd); bot_finish(d, 0, 0); return;               /* SYNCHRONIZE CACHE */
    case 0x1B: bot_finish(d, 0, 0); return;                              /* START STOP UNIT */
    default:
        set_sense(d, 5, 0x20, 0);                                        /* ILLEGAL REQUEST */
        bot_finish(d, dlen, 1); return;
    }
}

/* ================================================================== */
/*  xHC: стан                                                          */
/* ================================================================== */
typedef struct { int valid; uint64_t deq; uint32_t dcs; int state; uint32_t mps; } ep_t;
typedef struct { int enabled; int addressed; int port; vdev_t *dev; uint8_t *out; ep_t ep[32]; int entries; } slot_t;

static slot_t slots[NSLOTS + 1];
static uint64_t dcbaap, cmd_deq, erstba, erdp;
static uint32_t cmd_cycle, ev_cycle, ev_enq, erstsz;
static int running_dma(void) { return (rd_cfg32(0x04) & 4) != 0; }

static uint32_t rd32m(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static void wr32m(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static uint32_t *regp(uint32_t off) { return (uint32_t *)(regs + off); }

static void post_event(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t type, uint32_t extra_ctl) {
    if (!erstsz) return;
    uint64_t seg = ((uint64_t)rd32m(erstba + 4) << 32) | rd32m(erstba);
    uint32_t size = rd32m(erstba + 8);
    uint64_t a = seg + (uint64_t)ev_enq * 16;
    wr32m(a, d0); wr32m(a + 4, d1); wr32m(a + 8, d2);
    __asm__ volatile ("" ::: "memory");
    wr32m(a + 12, (type << 10) | extra_ctl | ev_cycle);
    emu.events_posted++;
    if (++ev_enq == size) { ev_enq = 0; ev_cycle ^= 1; }
    *regp(OPBASE + 4) |= (1u << 3);          /* USBSTS.EINT */
}

static void post_cmd_done(uint64_t trb, uint32_t cc, uint32_t slot) {
    post_event((uint32_t)trb, (uint32_t)(trb >> 32), cc << 24, 33, slot << 24);
}
static void post_xfer(uint64_t trb, uint32_t cc, uint32_t resid, uint32_t slot, uint32_t dci) {
    post_event((uint32_t)trb, (uint32_t)(trb >> 32), (cc << 24) | (resid & 0xFFFFFF), 32, (dci << 16) | (slot << 24));
}

/* контексти: розмір контексту 32 або 64 */
static uint8_t *ctxp(uint8_t *base, int n) { return base + (size_t)n * emu.csz; }

static void set_ep_state(slot_t *s, int dci, int state) {
    s->ep[dci].state = state;
    uint32_t *e = (uint32_t *)ctxp(s->out, dci);
    e[0] = (e[0] & ~7u) | (uint32_t)state;
}

/* ================================================================== */
/*  Команди                                                            */
/* ================================================================== */
static void cmd_address(uint64_t trb, uint32_t ctl) {
    uint32_t sid = ctl >> 24;
    uint64_t inp = ((uint64_t)rd32m(trb + 4) << 32) | rd32m(trb);
    emu.cmds++;
    if (sid < 1 || sid > NSLOTS || !slots[sid].enabled) { post_cmd_done(trb, 11, sid); return; }
    slot_t *s = &slots[sid];
    uint8_t *in = (uint8_t *)(uintptr_t)inp;
    uint32_t drop = *(uint32_t *)ctxp(in, 0), add = *(uint32_t *)(ctxp(in, 0) + 4);
    uint32_t *isl = (uint32_t *)ctxp(in, 1);
    uint32_t *iep0 = (uint32_t *)ctxp(in, 2);
    uint64_t out = dcbaap ? (((uint64_t)rd32m(dcbaap + sid * 8 + 4) << 32) | rd32m(dcbaap + sid * 8)) : 0;

    /* проверки як у справжнього xHC */
    if (drop != 0 || add != 3 || !out || (out & 63)) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    if ((isl[0] >> 27) != 1) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    int port = (isl[1] >> 16) & 0xFF;
    if (port < 1 || port > NPORTS || !ports[port].present) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    vdev_t *d = &ports[port];
    if ((int)((isl[0] >> 20) & 0xF) != d->speed) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    if (((iep0[1] >> 3) & 7) != 4 || !(iep0[2] & ~0xFu)) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    uint32_t mps = iep0[1] >> 16;
    if (d->speed == 3 && mps != 64) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    if (d->speed == 4 && mps != 512) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    if (d->speed <= 2 && mps != 8 && mps != 16 && mps != 32 && mps != 64) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }

    s->out = (uint8_t *)(uintptr_t)out;
    memset(s->out, 0, (size_t)32 * emu.csz);
    memcpy(ctxp(s->out, 0), isl, 32);
    memcpy(ctxp(s->out, 1), iep0, 32);
    uint32_t *osl = (uint32_t *)ctxp(s->out, 0);
    d->address = (int)sid;
    osl[3] = (2u << 27) | sid;
    s->port = port; s->dev = d; s->addressed = 1; s->entries = 1;
    s->ep[1].valid = 1;
    s->ep[1].deq = ((uint64_t)iep0[3] << 32) | (iep0[2] & ~0xFu);
    s->ep[1].dcs = iep0[2] & 1;
    s->ep[1].mps = mps;
    set_ep_state(s, 1, 1);
    post_cmd_done(trb, 1, sid);
}

static void cmd_configure(uint64_t trb, uint32_t ctl) {
    uint32_t sid = ctl >> 24;
    uint64_t inp = ((uint64_t)rd32m(trb + 4) << 32) | rd32m(trb);
    emu.cmds++;
    if (sid < 1 || sid > NSLOTS || !slots[sid].addressed) { post_cmd_done(trb, 19, sid); return; }
    slot_t *s = &slots[sid];
    uint8_t *in = (uint8_t *)(uintptr_t)inp;
    uint32_t drop = *(uint32_t *)ctxp(in, 0), add = *(uint32_t *)(ctxp(in, 0) + 4);
    uint32_t *isl = (uint32_t *)ctxp(in, 1);
    if (add & 2) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }            /* EP0 не додається */
    if (!(add & 1)) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }         /* ми завжди оновлюємо slot ctx */
    int entries = (int)(isl[0] >> 27);
    for (int dci = 2; dci < 32; dci++) {
        if (!(add & (1u << dci))) continue;
        if (dci > entries) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
        uint32_t *ie = (uint32_t *)ctxp(in, dci + 1);
        int type = (ie[1] >> 3) & 7;
        if (type != 2 && type != 6 && type != 7) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
        if ((dci & 1) != (type == 6 || type == 7)) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }   /* IN має непарний DCI */
        if (!(ie[2] & ~0xFu) || (ie[1] >> 16) == 0) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    }
    (void)drop;
    /* прийняти */
    memcpy(ctxp(s->out, 0), isl, 16);                 /* slot ctx dw0..3 (стан/адреса лишаємо) */
    uint32_t *osl = (uint32_t *)ctxp(s->out, 0);
    osl[3] = (3u << 27) | sid;
    s->entries = entries;
    for (int dci = 2; dci < 32; dci++) {
        if (!(add & (1u << dci))) continue;
        uint32_t *ie = (uint32_t *)ctxp(in, dci + 1);
        memcpy(ctxp(s->out, dci), ie, 32);
        s->ep[dci].valid = 1;
        s->ep[dci].deq = ((uint64_t)ie[3] << 32) | (ie[2] & ~0xFu);
        s->ep[dci].dcs = ie[2] & 1;
        s->ep[dci].mps = ie[1] >> 16;
        set_ep_state(s, dci, 1);
    }
    post_cmd_done(trb, 1, sid);
}

static void cmd_evaluate(uint64_t trb, uint32_t ctl) {
    uint32_t sid = ctl >> 24;
    uint64_t inp = ((uint64_t)rd32m(trb + 4) << 32) | rd32m(trb);
    emu.cmds++;
    if (sid < 1 || sid > NSLOTS || !slots[sid].addressed) { post_cmd_done(trb, 19, sid); return; }
    slot_t *s = &slots[sid];
    uint8_t *in = (uint8_t *)(uintptr_t)inp;
    uint32_t add = *(uint32_t *)(ctxp(in, 0) + 4);
    if (add & ~3u) { emu.param_errors++; post_cmd_done(trb, 17, sid); return; }
    if (add & 2) {
        uint32_t *iep0 = (uint32_t *)ctxp(in, 2);
        uint32_t mps = iep0[1] >> 16;
        uint32_t *oep0 = (uint32_t *)ctxp(s->out, 1);
        oep0[1] = (oep0[1] & 0xFFFFu) | (mps << 16);
        s->ep[1].mps = mps;
        emu.mps0_updates++;
    }
    post_cmd_done(trb, 1, sid);
}

static void cmd_ep(uint64_t trb, uint32_t ctl, int type) {
    uint32_t sid = ctl >> 24, dci = (ctl >> 16) & 0x1F;
    emu.cmds++;
    if (sid < 1 || sid > NSLOTS || !slots[sid].addressed) { post_cmd_done(trb, 11, sid); return; }
    slot_t *s = &slots[sid];
    if (dci < 1 || !s->ep[dci].valid) { post_cmd_done(trb, 12, sid); return; }
    if (type == 14) {                                   /* Reset Endpoint */
        if (s->ep[dci].state != 2) { post_cmd_done(trb, 19, sid); return; }
        set_ep_state(s, dci, 3);
        emu.ep_resets++;
    } else {                                            /* Stop Endpoint */
        if (s->ep[dci].state != 1) { post_cmd_done(trb, 19, sid); return; }
        set_ep_state(s, dci, 3);
        emu.ep_stops++;
        /* реальний контролер повідомляє про зупинений TD подією "Stopped" */
        post_xfer(s->ep[dci].deq, 26, 0, sid, dci);
    }
    post_cmd_done(trb, 1, sid);
}

static void cmd_set_deq(uint64_t trb, uint32_t ctl) {
    uint32_t sid = ctl >> 24, dci = (ctl >> 16) & 0x1F;
    uint32_t d0 = rd32m(trb), d1 = rd32m(trb + 4);
    emu.cmds++;
    if (sid < 1 || sid > NSLOTS || !slots[sid].addressed) { post_cmd_done(trb, 11, sid); return; }
    slot_t *s = &slots[sid];
    if (dci < 1 || !s->ep[dci].valid) { post_cmd_done(trb, 12, sid); return; }
    if (s->ep[dci].state != 3) { post_cmd_done(trb, 19, sid); return; }     /* лише Stopped/Error */
    if (d0 & 0xE) { post_cmd_done(trb, 17, sid); return; }
    s->ep[dci].deq = ((uint64_t)d1 << 32) | (d0 & ~0xFu);
    s->ep[dci].dcs = d0 & 1;
    emu.set_deqs++;
    post_cmd_done(trb, 1, sid);
}

static void process_commands(void) {
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t d0 = rd32m(cmd_deq), d1 = rd32m(cmd_deq + 4), d2 = rd32m(cmd_deq + 8), d3 = rd32m(cmd_deq + 12);
        (void)d2;
        if ((d3 & 1) != cmd_cycle) return;
        uint32_t type = (d3 >> 10) & 63;
        uint64_t me = cmd_deq;
        if (type == 6) {                                        /* Link */
            cmd_deq = ((uint64_t)d1 << 32) | d0;
            if (d3 & 2) cmd_cycle ^= 1;
            continue;
        }
        cmd_deq += 16;
        switch (type) {
        case 9: {                                               /* Enable Slot */
            uint32_t sid = 0;
            for (uint32_t i = 1; i <= NSLOTS; i++) if (!slots[i].enabled) { sid = i; break; }
            emu.cmds++;
            if (!sid) { post_cmd_done(me, 9, 0); break; }
            memset(&slots[sid], 0, sizeof(slots[sid]));
            slots[sid].enabled = 1;
            post_cmd_done(me, 1, sid);
            break;
        }
        case 10: {                                              /* Disable Slot */
            uint32_t sid = d3 >> 24;
            emu.cmds++;
            if (sid < 1 || sid > NSLOTS || !slots[sid].enabled) { post_cmd_done(me, 11, sid); break; }
            memset(&slots[sid], 0, sizeof(slots[sid]));
            emu.slots_disabled++;
            post_cmd_done(me, 1, sid);
            break;
        }
        case 11: cmd_address(me, d3); break;
        case 12: cmd_configure(me, d3); break;
        case 13: cmd_evaluate(me, d3); break;
        case 14: cmd_ep(me, d3, 14); break;
        case 15: cmd_ep(me, d3, 15); break;
        case 16: cmd_set_deq(me, d3); break;
        default: emu.bad_cmds++; post_cmd_done(me, 5, 0); break;
        }
    }
}

/* ================================================================== */
/*  Передачі                                                           */
/* ================================================================== */
typedef struct { uint64_t addr; uint32_t d0, d1, d2, d3; } trbv_t;

static trbv_t fetch(uint64_t a) {
    trbv_t t; t.addr = a; t.d0 = rd32m(a); t.d1 = rd32m(a + 4); t.d2 = rd32m(a + 8); t.d3 = rd32m(a + 12); return t;
}

/* Дістати наступний видимий TRB (з обробкою Link). 0 — нема */
static int next_trb(ep_t *e, trbv_t *t) {
    for (int g = 0; g < 8; g++) {
        *t = fetch(e->deq);
        if ((t->d3 & 1) != e->dcs) return 0;
        if (((t->d3 >> 10) & 63) == 6) {
            e->deq = ((uint64_t)t->d1 << 32) | t->d0;
            if (t->d3 & 2) e->dcs ^= 1;
            continue;
        }
        return 1;
    }
    return 0;
}

static void advance(ep_t *e) { e->deq += 16; }

static void ep_halt(slot_t *s, int dci) { set_ep_state(s, dci, 2); }

/* control TD */
static int process_control(slot_t *s, uint32_t sid) {
    ep_t *e = &s->ep[1];
    vdev_t *d = s->dev;
    trbv_t setup;
    if (!next_trb(e, &setup)) return 0;
    uint64_t td_start = e->deq;
    if (((setup.d3 >> 10) & 63) != 2 || !(setup.d3 & (1u << 6)) || (setup.d2 & 0x1FFFF) != 8) {
        emu.bad_trbs++; post_xfer(setup.addr, 5, 0, sid, 1); advance(e); return 1;
    }
    uint8_t sp[8];
    memcpy(sp, &setup.d0, 4); memcpy(sp + 4, &setup.d1, 4);
    uint32_t trt = (setup.d3 >> 16) & 3;
    uint16_t wlen = (uint16_t)(sp[6] | (sp[7] << 8));
    int in = (sp[0] & 0x80) != 0;
    advance(e);

    trbv_t data; int have_data = 0;
    if (trt) {
        if (!next_trb(e, &data)) { e->deq = td_start; return 0; }
        if (((data.d3 >> 10) & 63) != 3 || (((data.d3 >> 16) & 1) != (uint32_t)in) || (data.d2 & 0x1FFFF) != wlen || ((trt == 3) != in)) {
            emu.bad_trbs++; post_xfer(data.addr, 5, 0, sid, 1); advance(e); return 1;
        }
        have_data = 1; advance(e);
    } else if (wlen) { emu.bad_trbs++; post_xfer(setup.addr, 5, 0, sid, 1); return 1; }
    trbv_t status;
    if (!next_trb(e, &status)) { e->deq = td_start; return 0; }
    if (((status.d3 >> 10) & 63) != 4 || !(status.d3 & 0x20) ||
        (((status.d3 >> 16) & 1) != (uint32_t)((have_data && in) ? 0 : 1))) {
        emu.bad_trbs++; post_xfer(status.addr, 5, 0, sid, 1); advance(e); return 1;
    }
    advance(e);

    /* контролер сам шле SET_ADDRESS при Address Device; тут — запити драйвера */
    uint8_t reply[600]; int rlen = 0; int stall = 0;
    uint32_t mps_ctx = e->mps;
    uint8_t bm = sp[0], br = sp[1];
    uint16_t wval = (uint16_t)(sp[2] | (sp[3] << 8));
    uint16_t widx = (uint16_t)(sp[4] | (sp[5] << 8));
    emu.control_xfers++;

    /* EP0: якщо контролер вважає mps меншим за реальний (FS) — читати більше 8 байт не можна: babble */
    uint32_t real_mps = d->speed == 4 ? (1u << d->mps0) : d->mps0;
    int babble = 0;
    if (in && wlen > mps_ctx && mps_ctx < real_mps && d->speed <= 2) babble = 1;

    if (bm == 0x80 && br == 6) {                                           /* GET_DESCRIPTOR */
        int t = wval >> 8;
        if (t == 1) { uint8_t dd[18]; dev_descriptor(d, dd); rlen = wlen < 18 ? wlen : 18; memcpy(reply, dd, rlen); }
        else if (t == 2) { rlen = wlen < d->cfg_len ? wlen : d->cfg_len; memcpy(reply, d->cfg_desc, rlen); }
        else stall = 1;
    } else if (bm == 0x00 && br == 9) {                                    /* SET_CONFIGURATION */
        d->configured = wval;
        emu.set_configs++;
    } else if (bm == 0x21 && br == 0xFF && d->type == EMU_DEV_MSC) {       /* BOT reset */
        d->in_len = d->in_pos = 0; d->csw_ready = 0; d->expecting_data_out = 0;
        emu.bot_resets++;
    } else if (bm == 0x21 && br == 0x0B && d->type == EMU_DEV_MOUSE) {     /* SET_PROTOCOL */
        if (wval != 0) stall = 1; else emu.set_protocol++;
    } else if (bm == 0x21 && br == 0x0A && d->type == EMU_DEV_MOUSE) {     /* SET_IDLE */
        emu.set_idle++;
    } else if (bm == 0x02 && br == 1 && wval == 0 && d->type == EMU_DEV_MSC) { /* CLEAR_FEATURE(HALT) */
        emu.clear_halts++;
        (void)widx;
    } else {
        stall = 1;
    }
    if (d->configured == 0 && bm == 0x00 && br == 9) d->configured = 0;

    if (babble) {
        emu.babbles++;
        post_xfer(have_data ? data.addr : setup.addr, 3, 0, sid, 1);
        ep_halt(s, 1);
        return 1;
    }
    if (stall) {
        post_xfer(have_data ? data.addr : status.addr, 6, 0, sid, 1);
        ep_halt(s, 1);
        emu.stalls++;
        return 1;
    }
    if (have_data && in) {
        memcpy((void *)(uintptr_t)(((uint64_t)data.d1 << 32) | data.d0), reply, (size_t)rlen);
        if (rlen < wlen && (data.d3 & 4))                                  /* ISP: подія про short packet */
            post_xfer(data.addr, 13, wlen - rlen, sid, 1);
    }
    post_xfer(status.addr, 1, 0, sid, 1);
    return 1;
}

/* bulk TD: збираємо ланцюжок TRB до першого без CH */
static int process_bulk(slot_t *s, uint32_t sid, int dci) {
    ep_t *e = &s->ep[dci];
    vdev_t *d = s->dev;
    trbv_t trbs[8]; int n = 0;
    uint64_t td_start = e->deq;
    for (;;) {
        if (n >= 8) { emu.bad_trbs++; return 1; }
        if (!next_trb(e, &trbs[n])) { e->deq = td_start; return 0; }
        if (((trbs[n].d3 >> 10) & 63) != 1) { emu.bad_trbs++; post_xfer(trbs[n].addr, 5, 0, sid, dci); advance(e); return 1; }
        int ch = (trbs[n].d3 >> 4) & 1;
        advance(e);
        n++;
        if (!ch) break;
    }
    if (!(trbs[n - 1].d3 & 0x20)) { emu.bad_trbs++; post_xfer(trbs[n - 1].addr, 5, 0, sid, dci); return 1; }  /* IOC на останньому */

    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = trbs[i].d2 & 0x1FFFF;
        uint64_t addr = ((uint64_t)trbs[i].d1 << 32) | trbs[i].d0;
        if (len && (addr & ~0xFFFFull) != ((addr + len - 1) & ~0xFFFFull)) { emu.bad_trbs++; emu.crossed_64k++; }
        total += len;
    }
    int is_in = (dci & 1);
    emu.bulk_tds++;

    if (!d->present) { return 1; }                                         /* пристрій відключено: тиша -> таймаут */
    if (is_in && d->drop_in_next) { d->drop_in_next = 0; emu.dropped++; e->deq = td_start; /* лишаємо TD "зависшим" */
                                    return 1; }
    if (is_in && d->stall_in_next) { d->stall_in_next = 0; ep_halt(s, dci); emu.stalls++; post_xfer(trbs[0].addr, 6, total, sid, dci); return 1; }
    if (!is_in && d->stall_out_next) { d->stall_out_next = 0; ep_halt(s, dci); emu.stalls++; post_xfer(trbs[0].addr, 6, total, sid, dci); return 1; }

    if (!is_in) {                                                          /* OUT: дані від хоста */
        uint8_t *buf = d->out_buf; uint32_t pos = 0;
        for (int i = 0; i < n; i++) {
            uint32_t len = trbs[i].d2 & 0x1FFFF;
            memcpy(buf + pos, (void *)(uintptr_t)(((uint64_t)trbs[i].d1 << 32) | trbs[i].d0), len);
            pos += len;
        }
        if (d->expecting_data_out) {
            if (pos < d->out_expect) { emu.bad_trbs++; }
            run_scsi(d, d->pend_cbw, d->out_buf);
            d->expecting_data_out = 0;
        } else {
            if (pos != 31 || get_le32(buf) != 0x43425355u) { emu.bad_cbw++; }
            else {
                memcpy(d->pend_cbw, buf, 31);
                uint32_t dlen = get_le32(buf + 8);
                int dir_in = (buf[12] & 0x80) != 0;
                emu.cbws++;
                if (dlen && !dir_in) { d->expecting_data_out = 1; d->out_expect = dlen; d->tag = get_le32(buf + 4); }
                else run_scsi(d, buf, 0);
            }
        }
        post_xfer(trbs[n - 1].addr, 1, 0, sid, dci);
        return 1;
    }

    if (d->type == EMU_DEV_MOUSE) {                                        /* interrupt IN: звіт, якщо є */
        if (d->mq_n == 0) { e->deq = td_start; emu.naks++; return 0; }      /* NAK: TD чекає */
        uint8_t rep[4]; memcpy(rep, d->mq[0], 4);
        memmove(d->mq[0], d->mq[1], (size_t)(d->mq_n - 1) * 4); d->mq_n--;
        uint32_t give = total < 4 ? total : 4;
        memcpy((void *)(uintptr_t)(((uint64_t)trbs[0].d1 << 32) | trbs[0].d0), rep, give);
        emu.intr_reports++;
        if (total > give) post_xfer(trbs[n - 1].addr, 13, total - give, sid, dci);
        else post_xfer(trbs[n - 1].addr, 1, 0, sid, dci);
        return 1;
    }

    /* IN: або дані, або CSW */
    uint8_t chunk[70000]; uint32_t give = 0;
    if (d->in_pos < d->in_len) {
        give = d->in_len - d->in_pos; if (give > total) give = total;
        memcpy(chunk, d->in_data + d->in_pos, give);
        d->in_pos += give;
    } else if (d->csw_ready) {
        give = 13; if (give > total) give = total;
        memcpy(chunk, d->csw, give);
        if (give == 13) { d->csw_ready = 0; emu.csws++; }
    }
    /* розкласти по TRB */
    uint32_t pos = 0;
    int short_idx = -1; uint32_t short_resid = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = trbs[i].d2 & 0x1FFFF;
        uint32_t take = give - pos < len ? give - pos : len;
        if (take) memcpy((void *)(uintptr_t)(((uint64_t)trbs[i].d1 << 32) | trbs[i].d0), chunk + pos, take);
        pos += take;
        if (take < len && short_idx < 0) { short_idx = i; short_resid = len - take; }
    }
    if (give < total && give == 0 && !(d->in_pos < d->in_len)) {
        /* нічого віддати: справжній пристрій відповів би NAK -> таймаут на стороні хоста */
        e->deq = td_start; emu.naks++;
        return 1;
    }
    if (short_idx >= 0) post_xfer(trbs[short_idx].addr, 13, short_resid, sid, dci);
    else post_xfer(trbs[n - 1].addr, 1, 0, sid, dci);
    return 1;
}

static void ring_ep(uint32_t sid, uint32_t dci) {
    if (sid < 1 || sid > NSLOTS || !slots[sid].addressed || dci < 1 || dci > 31) { emu.bad_doorbells++; return; }
    slot_t *s = &slots[sid];
    ep_t *e = &s->ep[dci];
    if (!e->valid) { emu.bad_doorbells++; return; }
    if (e->state == 2) { emu.halted_doorbells++; return; }                 /* Halted: doorbell ігнорується */
    if (e->state == 3) set_ep_state(s, dci, 1);                            /* Stopped -> Running */
    for (int g = 0; g < 64; g++) {
        int r = dci == 1 ? process_control(s, sid) : process_bulk(s, sid, (int)dci);
        if (!r) break;
        if (e->state != 1) break;
        uint64_t before = e->deq; (void)before;
        trbv_t peek; if (!next_trb(e, &peek)) break;
    }
}

void emu_mouse_move(int port, int dx, int dy, int buttons, int wheel) {
    vdev_t *d = &ports[port];
    if (d->mq_n < 64) {
        d->mq[d->mq_n][0] = (uint8_t)buttons; d->mq[d->mq_n][1] = (uint8_t)(int8_t)dx;
        d->mq[d->mq_n][2] = (uint8_t)(int8_t)dy; d->mq[d->mq_n][3] = (uint8_t)(int8_t)wheel;
        d->mq_n++;
    }
    for (uint32_t sid = 1; sid <= NSLOTS; sid++)
        if (slots[sid].addressed && slots[sid].dev == d)
            for (uint32_t dci = 3; dci < 32; dci += 2)
                if (slots[sid].ep[dci].valid) ring_ep(sid, dci);
}


/* ================================================================== */
/*  Порти                                                              */
/* ================================================================== */
static int port_is_usb3(int p) { return p >= 3; }

static void port_init_all(void) {
    for (int p = 1; p <= NPORTS; p++) {
        uint32_t v = (1u << 9);                                            /* PP */
        if (ports[p].present) {
            v |= 1u | (1u << 17);                                          /* CCS + CSC */
            if (port_is_usb3(p)) { v |= 2u; v |= ((uint32_t)ports[p].speed << 10); /* PED, U0 */ }
            else v |= (7u << 5);                                           /* Polling: підключено, але не ввімкнено */
        } else if (port_is_usb3(p)) v |= (5u << 5);                        /* RxDetect */
        *regp(PORTSC(p)) = v;
    }
}

static void portsc_write(int p, uint32_t val) {
    uint32_t cur = *regp(PORTSC(p));
    uint32_t chg = 0x00FE0000u;
    cur &= ~(val & chg);                                                   /* RW1C */
    if (val & 2) cur &= ~2u;                                               /* PED: запис 1 вимикає */
    cur = (cur & ~(1u << 9)) | (val & (1u << 9));
    if (val & (1u << 4) && (cur & 1)) {                                    /* PR: hot reset */
        emu.port_resets++;
        cur |= 2u | (1u << 21);                                            /* PED + PRC */
        cur &= ~(0xFu << 5);                                               /* U0 */
        cur &= ~(0xFu << 10); cur |= ((uint32_t)ports[p].speed << 10);
        if (!ports[p].present) cur &= ~2u;
        ports[p].address = 0;
    }
    if ((val & (1u << 31)) && (cur & 1) && port_is_usb3(p)) {              /* WPR */
        emu.warm_resets++;
        cur |= 2u | (1u << 19);
        cur &= ~(0xFu << 5);
    }
    *regp(PORTSC(p)) = cur;
}

/* ================================================================== */
/*  MMIO                                                               */
/* ================================================================== */
static int regs_alive(void) {
    return (rd_cfg32(0x04) & 2) && ((rd_cfg32(0x74) & 3) == 0);
}

static void init_regs(void) {
    memset(regs, 0, sizeof(regs));
    uint32_t caplen_ver = CAPLEN | (0x0110u << 16);
    *regp(0x00) = caplen_ver;
    *regp(0x04) = NSLOTS | (1u << 8) | ((uint32_t)NPORTS << 24);
    *regp(0x08) = (4u << 4) | (2u << 27);                                   /* ERST max, scratchpad lo=2 */
    *regp(0x10) = (emu.csz == 64 ? (1u << 2) : 0) | ((uint32_t)(XECP / 4) << 16);
    *regp(0x14) = DBBASE;
    *regp(0x18) = RTBASE;
    *regp(OPBASE + 0) = emu.bios_running ? 1u : 0u;                          /* USBCMD: RS, якщо BIOS ще "працює" */
    *regp(OPBASE + 4) = emu.bios_running ? 0u : 1u;                          /* USBSTS.HCH */
    *regp(OPBASE + 8) = 1;                                                   /* PAGESIZE: 4 КБ */
    /* extended caps */
    *regp(XECP + 0x00) = 1u | (4u << 8) | (1u << 16);                        /* Legacy: BIOS owned */
    *regp(XECP + 0x10) = 2u | (4u << 8) | (2u << 24);
    *regp(XECP + 0x14) = 0x20425355u;
    *regp(XECP + 0x18) = 1u | (2u << 8);                                     /* порти 1..2 */
    *regp(XECP + 0x20) = 2u | (3u << 24);
    *regp(XECP + 0x24) = 0x20425355u;
    *regp(XECP + 0x28) = 3u | (2u << 8);                                     /* порти 3..4 */
    port_init_all();
}

uint32_t emu_rd32(uintptr_t addr) {
    uint32_t off = (uint32_t)(addr - (uintptr_t)regs);
    if (addr < (uintptr_t)regs || off >= REGS_SIZE) { fprintf(stderr, "EMU: MMIO read outside BAR %lx\n", (unsigned long)addr); abort(); }
    emu.mmio_reads++;
    if (!regs_alive()) return 0xFFFFFFFFu;
    if (off == OPBASE + 0x18 + 0 ) return *regp(off) & ~8u;                  /* CRCR: CRR=0, ptr не читається */
    return *regp(off);
}

void emu_wr32(uintptr_t addr, uint32_t v) {
    uint32_t off = (uint32_t)(addr - (uintptr_t)regs);
    if (addr < (uintptr_t)regs || off >= REGS_SIZE) { fprintf(stderr, "EMU: MMIO write outside BAR %lx\n", (unsigned long)addr); abort(); }
    emu.mmio_writes++;
    if (!regs_alive()) { emu.writes_while_off++; return; }

    if (off == OPBASE + 0x00) {                                              /* USBCMD */
        uint32_t old = *regp(off);
        if (v & 2) {                                                         /* HCRST */
            if (!(*regp(OPBASE + 4) & 1)) emu.reset_while_running++;         /* спец.: скидати можна тільки зупиненим */
            uint32_t cmd_saved = 0; (void)cmd_saved;
            emu.hc_resets++;
            memset(slots, 0, sizeof(slots));
            dcbaap = cmd_deq = erstba = erdp = 0; cmd_cycle = ev_cycle = 1; ev_enq = 0; erstsz = 0;
            emu.bios_running = 0;
            *regp(OPBASE + 0) = 0; *regp(OPBASE + 4) = 1;                    /* HCH=1, CNR=0 */
            *regp(OPBASE + 0x18) = *regp(OPBASE + 0x1C) = 0;
            port_init_all();
            return;
        }
        *regp(off) = v & ~2u;
        if ((v & 1) && !(old & 1)) {                                         /* RS 0->1 */
            if (!emu_hc_ready_for_run()) { emu.bad_run++; }
            *regp(OPBASE + 4) &= ~1u;
        }
        if (!(v & 1) && (old & 1)) *regp(OPBASE + 4) |= 1u;
        return;
    }
    if (off == OPBASE + 4) { *regp(off) &= ~(v & ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 10))); return; }   /* RW1C */
    if (off == OPBASE + 8) return;                                           /* PAGESIZE RO */
    if (off == OPBASE + 0x38 + 0) {}                                         /* CRCR lo handled below */
    if (off == OPBASE + 0x18) { cmd_deq = (cmd_deq & ~0xFFFFFFFFull) | (v & ~0x3Fu); cmd_cycle = v & 1; *regp(off) = v; return; }
    if (off == OPBASE + 0x1C) { cmd_deq = (cmd_deq & 0xFFFFFFFFull) | ((uint64_t)v << 32); *regp(off) = v; return; }
    if (off == OPBASE + 0x30) { dcbaap = (dcbaap & ~0xFFFFFFFFull) | (v & ~0x3Fu); *regp(off) = v; return; }
    if (off == OPBASE + 0x34) { dcbaap = (dcbaap & 0xFFFFFFFFull) | ((uint64_t)v << 32); *regp(off) = v; return; }
    if (off == OPBASE + 0x38) { *regp(off) = v & 0xFF; return; }            /* CONFIG */
    if (off >= OPBASE + 0x400 && off < OPBASE + 0x400 + 0x10 * NPORTS && ((off - OPBASE - 0x400) % 0x10) == 0) {
        portsc_write((int)((off - OPBASE - 0x400) / 0x10) + 1, v);
        return;
    }
    if (off == RTBASE + 0x20) { *regp(off) = (*regp(off) & ~2u) | (v & 2u); if (v & 1) *regp(off) &= ~1u; return; }   /* IMAN: IP RW1C, IE */
    if (off == RTBASE + 0x28) { erstsz = v & 0xFFFF; *regp(off) = v; return; }
    if (off == RTBASE + 0x30) { erstba = (erstba & ~0xFFFFFFFFull) | (v & ~0x3Full); *regp(off) = v;
                                ev_cycle = 1; ev_enq = 0; return; }
    if (off == RTBASE + 0x34) { erstba = (erstba & 0xFFFFFFFFull) | ((uint64_t)v << 32); *regp(off) = v; return; }
    if (off == RTBASE + 0x38) { erdp = (erdp & ~0xFFFFFFFFull) | (v & ~0xFu); *regp(off) = v & ~8u; return; }
    if (off == RTBASE + 0x3C) { erdp = (erdp & 0xFFFFFFFFull) | ((uint64_t)v << 32); *regp(off) = v; return; }
    if (off == XECP + 0x00) {                                                /* USB Legacy Support: OS owned -> BIOS відпускає */
        uint32_t cur = *regp(off);
        if (v & (1u << 24)) { cur |= (1u << 24); cur &= ~(1u << 16); emu.handoffs++; }
        *regp(off) = cur;
        return;
    }
    if (off == XECP + 0x04) { *regp(off) = (*regp(off) & ~(v & (7u << 29))) | (v & 0x1FFFFFFFu); return; }
    if (off >= DBBASE && off < DBBASE + 4 * (NSLOTS + 1)) {                  /* Doorbell */
        if (!running_dma()) { emu.doorbell_no_bme++; return; }
        if (*regp(OPBASE + 4) & 1) { emu.doorbell_halted++; return; }        /* контролер зупинено */
        uint32_t n = (off - DBBASE) / 4;
        emu.doorbells++;
        if (n == 0) process_commands(); else ring_ep(n, v & 0xFF);
        return;
    }
    if (off < OPBASE) return;                                                /* capability — RO */
    *regp(off) = v;
}

/* RS=1 допустимий, лише якщо драйвер налаштував усе необхідне */
int emu_hc_ready_for_run(void) {
    return dcbaap && cmd_deq && erstba && erstsz == 1 && (*regp(OPBASE + 0x38) & 0xFF) > 0;
}

/* ================================================================== */
/*  Ініціалізація "пристрою"                                           */
/* ================================================================== */
void emu_power_on(int csz, int bios_running) {
    emu.csz = csz;
    emu.bios_running = bios_running;
    memset(cfg, 0, sizeof(cfg));
    wr_cfg32(0x00, 0x9DED8086u);
    wr_cfg32(0x08, (0x0Cu << 24) | (0x03u << 16) | (0x30u << 8));
    wr_cfg32(0x10, (uint32_t)(uintptr_t)regs | 4u);                          /* 64-бітний MMIO BAR0 */
    wr_cfg32(0x14, 0);
    wr_cfg32(0x04, 0x00100000u);                                             /* статус: capabilities list; command = 0 */
    cfg[0x34] = 0x70;
    wr_cfg32(0x70, 0x00000001u);                                             /* PM capability id 1, next 0 */
    wr_cfg32(0x74, 3u);                                                      /* D3hot */
    cmd_cycle = ev_cycle = 1; ev_enq = 0; erstsz = 0;
    dcbaap = cmd_deq = erstba = erdp = 0;
    memset(slots, 0, sizeof(slots));
    init_regs();
}
