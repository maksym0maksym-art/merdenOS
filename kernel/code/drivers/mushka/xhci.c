/*
 * xHCI host controller driver (polling).
 *
 * Мета: USB-флешки на сучасних ноутбуках (Intel PCH, AMD, ...) БЕЗ віртуалізації.
 * На таких машинах UHCI/EHCI немає взагалі: усі порти (і USB 2.0, і USB 3.x) віддає xHCI.
 *
 * Порядок роботи (за специфікацією xHCI 1.x, розд. 4.2 "Host Controller Initialization"):
 *   1. PCI: D0, Memory Space + Bus Master, INTx вимкнено (ми працюємо полінгом)
 *   2. Забрати контролер у BIOS (USB Legacy Support extended capability)
 *   3. Зупинити, HCRST, дочекатись CNR=0
 *   4. DCBAA, Scratchpad, Command Ring, Event Ring (1 сегмент), CONFIG, RS=1
 *   5. Для кожного порту з пристроєм: reset -> Enable Slot -> Address Device ->
 *      дескриптори -> probe() (наприклад Mass Storage) -> Configure Endpoint
 *
 * Уся DMA-пам'ять — статичні масиви в .bss; ядро ідентично відображене (phys == virt),
 * тому адреса буфера і є фізична адреса. BAR0 вище 4 ГБ недосяжний у 32-бітному ядрі.
 */

#include <stdint.h>
#include "../../../headers/xhci.h"
#include "../../../headers/PCI.h"
#include "../../../headers/usb_dbg.h"

/* ================================================================== */
/*  Доступ до регістрів                                                */
/* ================================================================== */
#ifdef XHCI_HOSTTEST
extern uint32_t emu_rd32(uintptr_t addr);
extern void     emu_wr32(uintptr_t addr, uint32_t v);
#define RD32(a)     emu_rd32((uintptr_t)(a))
#define WR32(a, v)  emu_wr32((uintptr_t)(a), (uint32_t)(v))
#else
#define RD32(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define WR32(a, v)  (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))
#endif

static inline void cbarrier(void) { __asm__ volatile ("" ::: "memory"); }
static inline void mbarrier(void) {
#ifdef XHCI_HOSTTEST
    __asm__ volatile ("mfence" ::: "memory");
#else
    __asm__ volatile ("lock; addl $0,(%%esp)" ::: "memory", "cc");
#endif
}

/* Capability registers */
#define CAP_HCSPARAMS1   0x04
#define CAP_HCSPARAMS2   0x08
#define CAP_HCCPARAMS1   0x10
#define CAP_DBOFF        0x14
#define CAP_RTSOFF       0x18

/* Operational registers (від op base) */
#define OP_USBCMD        0x00
#define OP_USBSTS        0x04
#define OP_PAGESIZE      0x08
#define OP_CRCR          0x18
#define OP_DCBAAP        0x30
#define OP_CONFIG        0x38
#define OP_PORTSC(n)     (0x400 + 0x10 * ((n) - 1))

#define CMD_RS           (1u << 0)
#define CMD_HCRST        (1u << 1)
#define CMD_INTE         (1u << 2)
#define STS_HCH          (1u << 0)
#define STS_HSE          (1u << 2)
#define STS_EINT         (1u << 3)
#define STS_PCD          (1u << 4)
#define STS_CNR          (1u << 11)
#define CRCR_RCS         (1u << 0)

/* Runtime registers: interrupter 0 */
#define RT_IMAN          0x20
#define RT_IMOD          0x24
#define RT_ERSTSZ        0x28
#define RT_ERSTBA        0x30
#define RT_ERDP          0x38

/* PORTSC */
#define PORTSC_CCS       (1u << 0)
#define PORTSC_PED       (1u << 1)
#define PORTSC_PR        (1u << 4)
#define PORTSC_PP        (1u << 9)
#define PORTSC_WPR       (1u << 31)
#define PORTSC_WRC       (1u << 19)
#define PORTSC_PRC       (1u << 21)
#define PORTSC_CHG_MASK  0x00FE0000u            /* CSC PEC WRC OCC PRC PLC CEC — RW1C */
#define PORTSC_KEEP_MASK (PORTSC_PP | 0x0000C000u | 0x0E000000u)   /* PP, PIC, WCE/WDE/WOE */
#define PORTSC_PLS(v)    (((v) >> 5) & 0xF)
#define PORTSC_SPEED(v)  (((v) >> 10) & 0xF)

/* TRB types */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIG_EP    12
#define TRB_EVAL_CTX     13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_TR_DEQ   16
#define EVT_TRANSFER     32
#define EVT_CMD_DONE     33
#define EVT_PORT_CHANGE  34

#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_CYCLE        (1u << 0)
#define TRB_TC           (1u << 1)
#define TRB_ISP          (1u << 2)
#define TRB_CH           (1u << 4)
#define TRB_IOC          (1u << 5)
#define TRB_IDT          (1u << 6)
#define TRB_DIR_IN       (1u << 16)
#define TRB_BSR          (1u << 9)

/* completion codes */
#define CC_SUCCESS       1
#define CC_STALL         6
#define CC_SHORT         13
#define CC_CTX_STATE     19

#define RING_TRBS        256
#define EV_TRBS          256
#define MAX_SCRATCH      64

/* ================================================================== */
/*  DMA-пам'ять                                                        */
/* ================================================================== */
typedef struct { volatile uint32_t d[4]; } trb_t;
typedef struct { trb_t trb[RING_TRBS]; } __attribute__((aligned(4096))) ring_mem_t;

typedef struct {
    ring_mem_t ep0, in, out;
    uint8_t    outctx[4096];       /* Device Context: до 32 контекстів по 32/64 байт */
    uint8_t    inctx[4096];        /* Input Context: до 33 контекстів */
} __attribute__((aligned(4096))) dev_mem_t;

static uint64_t   dcbaa[256]                    __attribute__((aligned(4096)));
static ring_mem_t cmd_ring_mem;
static ring_mem_t evt_ring_mem;
static struct { uint64_t base; uint32_t size; uint32_t rsvd; } erst[2] __attribute__((aligned(4096)));
static uint64_t   scratch_arr[MAX_SCRATCH]      __attribute__((aligned(4096)));
static uint8_t    scratch_pages[MAX_SCRATCH][4096] __attribute__((aligned(4096)));
static dev_mem_t  dev_mem[XHCI_MAX_DEVS];
static uint8_t    ctl_buf[1024]                 __attribute__((aligned(64)));

static xhci_dev_t devs[XHCI_MAX_DEVS];

static struct {
    int       up;                    /* контролер запущено */
    uintptr_t base, op, rt, db;
    uint32_t  hcs1, hcs2, hcc1;
    uint32_t  version;
    int       max_slots, max_ports, csz;
    int       nscratch;
    uint8_t   proto[256];            /* major revision (2/3) для кожного root-порту */
    pci_device_t pci;
    xhci_ring_t cmd;
    uint32_t  ev_deq, ev_cycle;
    uint32_t  ev_phys;
    uint32_t  port_events;
} x;

static uint32_t phys_of(const void *p) { return (uint32_t)(uintptr_t)p; }

static void zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static const char *speed_name(int s) {
    switch (s) { case 1: return "FS"; case 2: return "LS"; case 3: return "HS";
                 case 4: return "SS"; case 5: return "SS+"; }
    return "?";
}

static const char *cc_name(uint32_t cc) {
    switch (cc) {
    case 1: return "Success";            case 2: return "Data Buffer Error";
    case 3: return "Babble";             case 4: return "USB Transaction Error";
    case 5: return "TRB Error";          case 6: return "Stall";
    case 7: return "Resource Error";     case 8: return "Bandwidth Error";
    case 9: return "No Slots";           case 11: return "Slot Not Enabled";
    case 12: return "Endpoint Not Enabled"; case 13: return "Short Packet";
    case 17: return "Parameter Error";   case 19: return "Context State Error";
    case 21: return "Event Ring Full";   case 22: return "Incompatible Device";
    case 23: return "Missed Service";    case 24: return "Command Ring Stopped";
    case 26: return "Stopped";           case 27: return "Stopped - Length Invalid";
    case 35: return "Isoch Buffer Overrun"; case 36: return "Event Lost";
    }
    return "?";
}

/* ================================================================== */
/*  Кільця                                                             */
/* ================================================================== */
static void ring_init(xhci_ring_t *r, ring_mem_t *mem) {
    zero(mem, sizeof(*mem));
    r->trb   = &mem->trb[0].d[0];
    r->phys  = phys_of(mem);
    r->enq   = 0;
    r->cycle = 1;
    r->size  = RING_TRBS;

    volatile uint32_t *l = r->trb + (RING_TRBS - 1) * 4;
    l[0] = r->phys;
    l[1] = 0;
    l[2] = 0;
    l[3] = TRB_TYPE(TRB_LINK) | TRB_TC;      /* cycle-біт поставимо, коли enqueue дійде сюди */
}

/*
 * Поставити один TRB у кільце. Якщо defer != 0 — cycle-біт навмисно інвертований:
 * контролер не побачить TRB, поки ring_commit() не поставить правильний (так TD із кількох TRB
 * стає видимим цілком, а не частинами). Повертає фізичну адресу TRB.
 */
static uint32_t ring_push(xhci_ring_t *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, int defer) {
    volatile uint32_t *t = r->trb + r->enq * 4;
    uint32_t phys = r->phys + r->enq * 16;

    t[0] = d0; t[1] = d1; t[2] = d2;
    cbarrier();
    t[3] = (d3 & ~TRB_CYCLE) | (defer ? (r->cycle ^ 1u) : r->cycle);

    r->enq++;
    if (r->enq == r->size - 1) {
        /* дійшли до Link TRB: віддати його контролеру і перейти на початок із новим cycle */
        volatile uint32_t *l = r->trb + r->enq * 4;
        uint32_t ch = (d3 & TRB_CH) ? TRB_CH : 0;     /* Link усередині ланцюжка теж має CH */
        l[0] = r->phys; l[1] = 0; l[2] = 0;
        cbarrier();
        l[3] = TRB_TYPE(TRB_LINK) | TRB_TC | ch | r->cycle;
        r->enq = 0;
        r->cycle ^= 1u;
    }
    return phys;
}

/* Зробити відкладений TRB видимим (поставити правильний cycle). first_cycle — cycle на момент push. */
static void ring_commit(volatile uint32_t *first, uint32_t first_cycle) {
    mbarrier();
    first[3] = (first[3] & ~TRB_CYCLE) | first_cycle;
    mbarrier();
}

static void ring_doorbell(int slot, uint32_t target) {
    mbarrier();
    WR32(x.db + (uintptr_t)slot * 4, target);
}

/* ================================================================== */
/*  Event Ring                                                         */
/* ================================================================== */
static int ev_poll(uint32_t out[4]) {
    volatile uint32_t *e = &evt_ring_mem.trb[x.ev_deq].d[0];
    uint32_t ctl = e[3];
    if ((ctl & TRB_CYCLE) != x.ev_cycle) return 0;
    cbarrier();
    out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = ctl;

    x.ev_deq++;
    if (x.ev_deq == EV_TRBS) { x.ev_deq = 0; x.ev_cycle ^= 1u; }

    uint32_t p = x.ev_phys + x.ev_deq * 16;
    WR32(x.rt + RT_ERDP,     p | 8u);          /* EHB (bit 3) — write 1 to clear */
    WR32(x.rt + RT_ERDP + 4, 0);
    return 1;
}

/* Подія, яку не чекає поточна операція (інший пристрій/endpoint): зміна порту або завершення interrupt-TD. */
static int ev_dispatch_other(const uint32_t e[4]) {
    uint32_t type = (e[3] >> 10) & 63;
    if (type == EVT_PORT_CHANGE) { x.port_events++; return 1; }
    if (type != EVT_TRANSFER) return 0;
    int slot = (int)(e[3] >> 24), ep = (int)((e[3] >> 16) & 31);
    for (int i = 0; i < XHCI_MAX_DEVS; i++) {
        xhci_dev_t *d = &devs[i];
        if (d->used && d->intr_armed && !d->intr_done && d->slot == slot && d->dci_in == ep && d->intr_trb == e[0]) {
            d->intr_cc = e[2] >> 24;
            d->intr_resid = e[2] & 0xFFFFFF;
            d->intr_done = 1;
            return 1;
        }
    }
    return 0;
}

static void ev_drain(uint32_t ms) {
    uint32_t e[4];
    usb_deadline_t dl = usb_deadline_ms(ms);
    while (!usb_expired(dl)) {
        if (!ev_poll(e)) continue;
        ev_dispatch_other(e);
    }
}

/* ================================================================== */
/*  Команди                                                            */
/* ================================================================== */
static int cmd_submit(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                      uint32_t *cc_out, uint32_t *slot_out, uint32_t timeout_ms) {
    if (!x.up) return XHCI_E_NODEV;

    uint32_t phys = ring_push(&x.cmd, d0, d1, d2, d3, 0);
    ring_doorbell(0, 0);

    usb_deadline_t dl = usb_deadline_ms(timeout_ms);
    uint32_t e[4];
    while (!usb_expired(dl)) {
        if (!ev_poll(e)) continue;

        uint32_t type = (e[3] >> 10) & 63;
        if (type == EVT_CMD_DONE) {
            if (e[0] != phys) { USB_DBG("[xhci] stale cmd event %08x\n", e[0]); continue; }
            uint32_t cc = e[2] >> 24;
            if (cc_out) *cc_out = cc;
            if (slot_out) *slot_out = e[3] >> 24;
            USB_DBG("[xhci] cmd type %u -> %s (%u)\n", (d3 >> 10) & 63, cc_name(cc), cc);
            return 0;
        }
        if (!ev_dispatch_other(e)) USB_DBG("[xhci] event type %u while waiting for command\n", type);
    }
    usb_printf("[xhci] command type %u timed out\n", (d3 >> 10) & 63);
    return XHCI_E_TIMEOUT;
}

/* команда, яка має завершитись Success; повертає 0 або XHCI_E_* */
static int cmd_ok(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *slot_out, uint32_t timeout_ms) {
    uint32_t cc = 0;
    int r = cmd_submit(d0, d1, d2, d3, &cc, slot_out, timeout_ms);
    if (r) return r;
    if (cc != CC_SUCCESS) {
        usb_printf("[xhci] command type %u failed: %s (%u)\n", (d3 >> 10) & 63, cc_name(cc), cc);
        return XHCI_E_CMD;
    }
    return 0;
}

/* ================================================================== */
/*  Контексти                                                          */
/* ================================================================== */
static uint32_t *ictx(xhci_dev_t *d, int n) { return (uint32_t *)(dev_mem[d->idx].inctx + (uint32_t)n * (uint32_t)x.csz); }
static uint32_t *octx(xhci_dev_t *d, int n) { return (uint32_t *)(dev_mem[d->idx].outctx + (uint32_t)n * (uint32_t)x.csz); }

static ring_mem_t *dev_ring_mem(xhci_dev_t *d, int which) {
    dev_mem_t *m = &dev_mem[d->idx];
    return which == 0 ? &m->ep0 : which == 1 ? &m->in : &m->out;
}

static void ictx_clear(xhci_dev_t *d) { zero(dev_mem[d->idx].inctx, sizeof(dev_mem[d->idx].inctx)); }

static void ictx_copy_slot_from_output(xhci_dev_t *d) {
    uint32_t *src = octx(d, 0), *dst = ictx(d, 1);
    for (int i = 0; i < 8; i++) dst[i] = src[i];
}

static void ep_ctx_fill(uint32_t *ep, int type, uint32_t mps, uint32_t burst, const xhci_ring_t *ring, uint32_t avg) {
    ep[0] = 0;
    ep[1] = (3u << 1) | ((uint32_t)type << 3) | (burst << 8) | (mps << 16);
    ep[2] = ring->phys | 1u;                 /* TR Dequeue Pointer + DCS=1 */
    ep[3] = 0;
    ep[4] = avg;
}

/* ================================================================== */
/*  Керування портами                                                  */
/* ================================================================== */
static uintptr_t portsc_addr(int port) { return x.op + OP_PORTSC(port); }

static uint32_t portsc_neutral(uint32_t v) { return v & PORTSC_KEEP_MASK; }

/* Reset порту. Повертає speed id (>0) або XHCI_E_*. */
static int port_reset(int port) {
    uintptr_t reg = portsc_addr(port);
    uint32_t v = RD32(reg);
    if (!(v & PORTSC_CCS)) return XHCI_E_NODEV;

    int usb3 = (x.proto[port] == 3);

    WR32(reg, portsc_neutral(v) | PORTSC_CHG_MASK);          /* зняти старі прапорці змін */

    for (int attempt = 0; attempt < 2; attempt++) {
        int warm = attempt == 1;
        if (warm && !usb3) break;

        v = RD32(reg);
        WR32(reg, portsc_neutral(v) | (warm ? PORTSC_WPR : PORTSC_PR));

        usb_deadline_t dl = usb_deadline_ms(warm ? 1500 : 800);
        uint32_t done_bit = warm ? PORTSC_WRC : PORTSC_PRC;
        while (!usb_expired(dl)) {
            v = RD32(reg);
            if (v & done_bit) break;
            usb_delay_us(500);
        }
        v = RD32(reg);
        WR32(reg, portsc_neutral(v) | PORTSC_CHG_MASK);      /* PRC/WRC і решту — скинути */

        if (!(v & done_bit)) {
            USB_DBG("[xhci] port %d: %s reset timeout (PORTSC=%08x)\n", port, warm ? "warm" : "hot", v);
            continue;
        }
        v = RD32(reg);
        if (!(v & PORTSC_CCS)) return XHCI_E_NODEV;
        if (v & PORTSC_PED) {
            usb_delay_ms(usb3 ? 5 : 20);                     /* TRSTRCY */
            v = RD32(reg);
            int sp = (int)PORTSC_SPEED(v);
            if (sp == 0) sp = usb3 ? 4 : 3;
            return sp;
        }
        USB_DBG("[xhci] port %d: not enabled after reset (PORTSC=%08x PLS=%u)\n", port, v, PORTSC_PLS(v));
    }
    return XHCI_E_XFER;
}

/* ================================================================== */
/*  Передачі                                                           */
/* ================================================================== */
typedef struct {
    uint32_t phys[4];
    int      n;
    int      short_terminal;      /* 1: Short Packet завершує TD (bulk). 0: ні (control: далі йде Status) */
} td_t;

typedef struct {
    int      done_idx;            /* на якому TRB завершився TD */
    uint32_t resid;               /* residual того TRB */
    uint32_t data_resid;          /* для control: residual Data Stage (0, якщо подія не приходила) */
    int      have_data_resid;
    uint32_t cc;
} xres_t;

static int cc_to_err(uint32_t cc) {
    if (cc == CC_STALL) return XHCI_E_STALL;
    return XHCI_E_XFER;
}

static int td_index(const td_t *td, uint32_t phys) {
    for (int i = 0; i < td->n; i++) if (td->phys[i] == phys) return i;
    return -1;
}

static int xfer_wait(xhci_dev_t *d, int dci, const td_t *td, uint32_t timeout_ms, xres_t *res) {
    usb_deadline_t dl = usb_deadline_ms(timeout_ms);
    uint32_t e[4];
    res->have_data_resid = 0;
    res->data_resid = 0;
    res->cc = 0;

    while (!usb_expired(dl)) {
        if (!ev_poll(e)) continue;

        uint32_t type = (e[3] >> 10) & 63;
        if (type == EVT_PORT_CHANGE) { x.port_events++; continue; }
        if (type != EVT_TRANSFER) { USB_DBG("[xhci] event type %u during transfer\n", type); continue; }

        int slot = (int)(e[3] >> 24);
        int ep   = (int)((e[3] >> 16) & 31);
        uint32_t cc    = e[2] >> 24;
        uint32_t resid = e[2] & 0xFFFFFF;
        int idx = td_index(td, e[0]);

        if (slot != d->slot || ep != dci || idx < 0) {
            if (ev_dispatch_other(e)) continue;
            USB_DBG("[xhci] ignoring stale transfer event slot=%d ep=%d cc=%u trb=%08x\n", slot, ep, cc, e[0]);
            continue;
        }

        res->cc = cc;
        d->last_cc = cc;

        if (cc != CC_SUCCESS && cc != CC_SHORT) {
            res->done_idx = idx;
            res->resid = resid;
            return cc_to_err(cc);
        }
        if (td->n == 3 && idx == 1) {                   /* control: Data Stage */
            res->data_resid = resid;
            res->have_data_resid = 1;
        }
        if (idx == td->n - 1 || (cc == CC_SHORT && td->short_terminal)) {
            res->done_idx = idx;
            res->resid = resid;
            return 0;
        }
    }
    return XHCI_E_TIMEOUT;
}

/* Відновлення endpoint'а: Stop (якщо біжить) -> Reset (якщо Halted) -> Set TR Dequeue на enqueue. */
static int ep_recover(xhci_dev_t *d, int dci, xhci_ring_t *ring) {
    uint32_t state = octx(d, dci)[0] & 7;
    USB_DBG("[xhci] recover slot %d ep %d (state %u)\n", d->slot, dci, state);

    if (state == 1) {                                    /* Running -> Stopped */
        uint32_t cc = 0;
        cmd_submit(0, 0, 0, TRB_TYPE(TRB_STOP_EP) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), &cc, 0, 1000);
        ev_drain(3);                                     /* "Stopped" transfer event нам не потрібен */
        state = octx(d, dci)[0] & 7;
    }
    if (state == 2) {                                    /* Halted -> Stopped (скидає data toggle) */
        int r = cmd_ok(0, 0, 0, TRB_TYPE(TRB_RESET_EP) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0, 1000);
        if (r) return r;
    }

    uint32_t deq = ring->phys + ring->enq * 16;
    int r = cmd_ok(deq | ring->cycle, 0, 0,
                   TRB_TYPE(TRB_SET_TR_DEQ) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0, 1000);
    ev_drain(2);
    return r;
}

int xhci_recover_ep(xhci_dev_t *d, int is_in) {
    if (is_in) return d->dci_in  ? ep_recover(d, d->dci_in,  &d->ring_in)  : XHCI_E_ARGS;
    else       return d->dci_out ? ep_recover(d, d->dci_out, &d->ring_out) : XHCI_E_ARGS;
}

static int ctl_quiet;

int xhci_control(xhci_dev_t *d, uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength) {
    if (!d || !d->used || !x.up) return XHCI_E_NODEV;
    if (wLength > sizeof(ctl_buf)) return XHCI_E_ARGS;

    int in = (bmRequestType & 0x80) != 0;
    xhci_ring_t *r = &d->ep0;
    td_t td;
    td.n = 0;
    td.short_terminal = 0;

    if (wLength && !in) {
        uint8_t *s = (uint8_t *)data;
        for (uint32_t i = 0; i < wLength; i++) ctl_buf[i] = s[i];
    }

    /* Setup Stage (відкладений: видимий для контролера лише після commit) */
    uint32_t trt = wLength ? (in ? 3u : 2u) : 0u;
    volatile uint32_t *first = r->trb + r->enq * 4;
    uint32_t first_cycle = r->cycle;
    td.phys[td.n++] = ring_push(r,
        (uint32_t)bmRequestType | ((uint32_t)bRequest << 8) | ((uint32_t)wValue << 16),
        (uint32_t)wIndex | ((uint32_t)wLength << 16),
        8,
        TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16), 1);

    if (wLength) {
        td.phys[td.n++] = ring_push(r, phys_of(ctl_buf), 0, wLength,
                                    TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0) | TRB_ISP, 0);
    }
    uint32_t st_dir = (wLength && in) ? 0 : TRB_DIR_IN;
    td.phys[td.n++] = ring_push(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | st_dir, 0);

    /* Структура td для xfer_wait: Data Stage має індекс 1 лише коли n == 3 */
    ring_commit(first, first_cycle);
    ring_doorbell(d->slot, 1);

    xres_t res;
    int rc = xfer_wait(d, 1, &td, 2000, &res);
    if (rc != 0) {
        if (!ctl_quiet)
            usb_printf("[xhci] control %02x/%02x failed: %s (cc=%u)\n", bmRequestType, bRequest,
                       rc == XHCI_E_TIMEOUT ? "timeout" : cc_name(res.cc), res.cc);
        ep_recover(d, 1, &d->ep0);
        return rc;
    }

    uint32_t got = wLength;
    if (res.have_data_resid && res.data_resid <= wLength) got = wLength - res.data_resid;
    if (wLength && in) {
        uint8_t *dst = (uint8_t *)data;
        for (uint32_t i = 0; i < got; i++) dst[i] = ctl_buf[i];
    }
    return (int)got;
}

static int bulk_submit(xhci_dev_t *d, int is_in, void *buf, uint32_t len, uint32_t *actual, uint32_t timeout_ms) {
    int dci = is_in ? d->dci_in : d->dci_out;
    xhci_ring_t *r = is_in ? &d->ring_in : &d->ring_out;
    if (!dci) return XHCI_E_ARGS;
    if (len > 65536) return XHCI_E_ARGS;

    uint32_t mps = is_in ? d->mps_in : d->mps_out;
    if (mps == 0) mps = 512;

    td_t td;
    td.n = 0;
    td.short_terminal = 1;
    uint32_t lens[4];

    volatile uint32_t *first = r->trb + r->enq * 4;
    uint32_t first_cycle = r->cycle;

    uint32_t addr = phys_of(buf);
    uint32_t remaining = len;
    do {
        uint32_t chunk = 0x10000u - (addr & 0xFFFFu);       /* TRB не може перетинати межу 64 КБ */
        if (chunk > remaining) chunk = remaining;
        remaining -= chunk;

        uint32_t pk = (remaining + mps - 1) / mps;
        if (pk > 31) pk = 31;

        uint32_t ctl = TRB_TYPE(TRB_NORMAL) | TRB_ISP | (remaining ? TRB_CH : TRB_IOC);
        lens[td.n] = chunk;
        td.phys[td.n] = ring_push(r, addr, 0, chunk | (pk << 17), ctl, td.n == 0);
        td.n++;
        addr += chunk;
    } while (remaining && td.n < 4);

    ring_commit(first, first_cycle);
    ring_doorbell(d->slot, (uint32_t)dci);

    xres_t res;
    int rc = xfer_wait(d, dci, &td, timeout_ms, &res);
    if (rc != 0) {
        if (actual) {
            uint32_t sent = 0;
            for (int i = 0; i < res.done_idx && i < td.n; i++) sent += lens[i];
            *actual = (rc == XHCI_E_TIMEOUT) ? 0 : sent + lens[res.done_idx] - res.resid;
        }
        return rc;
    }

    if (actual) {
        uint32_t sent = 0;
        for (int i = 0; i < res.done_idx; i++) sent += lens[i];
        *actual = sent + lens[res.done_idx] - res.resid;
    }
    return 0;
}

int xhci_bulk(xhci_dev_t *d, int is_in, void *buf, uint32_t len, uint32_t *actual, uint32_t timeout_ms) {
    if (!d || !d->used || !x.up) return XHCI_E_NODEV;
    int rc = bulk_submit(d, is_in, buf, len, actual, timeout_ms);
    if (rc == XHCI_E_TIMEOUT) {
        usb_printf("[xhci] bulk %s %u bytes timed out\n", is_in ? "IN" : "OUT", len);
        xhci_recover_ep(d, is_in);
    } else if (rc == XHCI_E_XFER) {
        usb_printf("[xhci] bulk %s error: %s\n", is_in ? "IN" : "OUT", cc_name(d->last_cc));
        xhci_recover_ep(d, is_in);
    }
    /* STALL: рішення (clear halt + прочитати CSW) приймає шар Mass Storage */
    return rc;
}

int xhci_clear_halt(xhci_dev_t *d, int is_in) {
    int dci = is_in ? d->dci_in : d->dci_out;
    uint8_t addr = is_in ? d->ep_in_addr : d->ep_out_addr;
    if (!dci) return XHCI_E_ARGS;

    int r = xhci_recover_ep(d, is_in);
    if (r) return r;
    r = xhci_control(d, 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, addr, 0, 0);
    return r < 0 ? r : 0;
}

/* ================================================================== */
/*  Пристрої                                                           */
/* ================================================================== */
static xhci_dev_t *dev_alloc(void) {
    for (int i = 0; i < XHCI_MAX_DEVS; i++) {
        if (devs[i].used) continue;
        zero(&devs[i], sizeof(devs[i]));
        devs[i].used = 1;
        devs[i].idx = i;
        zero(&dev_mem[i].outctx, sizeof(dev_mem[i].outctx));
        zero(&dev_mem[i].inctx, sizeof(dev_mem[i].inctx));
        return &devs[i];
    }
    return 0;
}

static void dev_release(xhci_dev_t *d) {
    if (d->slot) {
        cmd_submit(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)d->slot << 24), 0, 0, 1000);
        dcbaa[d->slot] = 0;
    }
    zero(d, sizeof(*d));
}

static uint32_t initial_mps0(int speed) {
    switch (speed) {
    case 4: case 5: return 512;      /* SuperSpeed: фіксовано 512 */
    case 3:         return 64;       /* HighSpeed: фіксовано 64 */
    default:        return 8;        /* FS: 8/16/32/64 — уточнимо після першого читання */
    }
}

/* Enable Slot + Address Device. */
static int dev_address(xhci_dev_t *d) {
    uint32_t slot = 0;
    int r = cmd_ok(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot, 1000);
    if (r) return r;
    if (slot == 0 || slot > 255) return XHCI_E_CMD;
    d->slot = (int)slot;

    ring_init(&d->ep0, dev_ring_mem(d, 0));
    d->mps0 = (uint16_t)initial_mps0(d->speed);

    zero(dev_mem[d->idx].outctx, sizeof(dev_mem[d->idx].outctx));
    dcbaa[d->slot] = phys_of(dev_mem[d->idx].outctx);

    ictx_clear(d);
    uint32_t *ctl = ictx(d, 0);
    ctl[0] = 0;
    ctl[1] = 0x3;                                   /* A0 (slot) | A1 (EP0) */

    uint32_t *sl = ictx(d, 1);
    sl[0] = (1u << 27) | ((uint32_t)d->speed << 20);   /* Context Entries = 1, speed */
    sl[1] = (uint32_t)d->port << 16;                   /* Root Hub Port Number */

    ep_ctx_fill(ictx(d, 2), 4 /* Control */, d->mps0, 0, &d->ep0, 8);

    r = cmd_ok(phys_of(dev_mem[d->idx].inctx), 0, 0,
               TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)d->slot << 24), 0, 3000);
    if (r) return r;

    usb_delay_ms(10);                               /* час на SET_ADDRESS у пристрої */
    return 0;
}

static int dev_fix_mps0(xhci_dev_t *d, uint32_t mps) {
    ictx_clear(d);
    uint32_t *ctl = ictx(d, 0);
    ctl[1] = 0x3;
    ictx_copy_slot_from_output(d);
    uint32_t *ep0 = ictx(d, 2), *src = octx(d, 1);
    for (int i = 0; i < 8; i++) ep0[i] = src[i];
    ep0[1] = (ep0[1] & 0x0000FFFFu) | (mps << 16);

    int r = cmd_ok(phys_of(dev_mem[d->idx].inctx), 0, 0,
                   TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t)d->slot << 24), 0, 1000);
    if (r == 0) d->mps0 = (uint16_t)mps;
    return r;
}

int xhci_configure_bulk(xhci_dev_t *d, uint8_t config_value,
                        uint8_t in_addr, uint16_t in_mps, uint8_t in_burst,
                        uint8_t out_addr, uint16_t out_mps, uint8_t out_burst) {
    if (!d || !d->used) return XHCI_E_NODEV;
    int dci_in  = (in_addr & 0x0F) * 2 + 1;
    int dci_out = (out_addr & 0x0F) * 2;
    if (dci_in < 2 || dci_in > 31 || dci_out < 2 || dci_out > 31) return XHCI_E_ARGS;

    ring_init(&d->ring_in,  dev_ring_mem(d, 1));
    ring_init(&d->ring_out, dev_ring_mem(d, 2));

    ictx_clear(d);
    uint32_t *ctl = ictx(d, 0);
    ctl[0] = 0;
    ctl[1] = 1u | (1u << dci_in) | (1u << dci_out);

    ictx_copy_slot_from_output(d);
    int last = dci_in > dci_out ? dci_in : dci_out;
    uint32_t *sl = ictx(d, 1);
    sl[0] = (sl[0] & 0x07FFFFFFu) | ((uint32_t)last << 27);

    ep_ctx_fill(ictx(d, 1 + dci_in),  6 /* Bulk IN  */, in_mps,  in_burst,  &d->ring_in,  in_mps);
    ep_ctx_fill(ictx(d, 1 + dci_out), 2 /* Bulk OUT */, out_mps, out_burst, &d->ring_out, out_mps);

    int r = cmd_ok(phys_of(dev_mem[d->idx].inctx), 0, 0,
                   TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24), 0, 2000);
    if (r) return r;

    d->dci_in = dci_in;       d->dci_out = dci_out;
    d->ep_in_addr = in_addr;  d->ep_out_addr = out_addr;
    d->mps_in = in_mps;       d->mps_out = out_mps;

    int c = xhci_control(d, 0x00, 9 /* SET_CONFIGURATION */, config_value, 0, 0, 0);
    if (c < 0) return c;
    return 0;
}

/* ---- Interrupt IN ---- */
int xhci_configure_intr_in(xhci_dev_t *d, uint8_t config_value, uint8_t ep_addr, uint16_t mps, uint8_t binterval) {
    if (!d || !d->used) return XHCI_E_NODEV;
    int dci = (ep_addr & 0x0F) * 2 + 1;
    if (!(ep_addr & 0x80) || dci < 3 || dci > 31 || mps == 0) return XHCI_E_ARGS;

    /* Interval в контексті — степінь двійки в одиницях 125 мкс */
    uint32_t iv;
    if (d->speed <= 2) {                                      /* LS/FS: binterval у мс */
        uint32_t frames = (uint32_t)(binterval ? binterval : 8) * 8, e = 0;
        while ((2u << e) <= frames) e++;                      /* floor(log2(frames)) */
        iv = e < 3 ? 3 : e > 10 ? 10 : e;
    } else {                                                  /* HS/SS: 2^(binterval-1) мікрокадрів */
        iv = binterval ? (uint32_t)binterval - 1u : 0u;
        if (iv > 15) iv = 15;
    }

    ring_init(&d->ring_in, dev_ring_mem(d, 1));

    ictx_clear(d);
    uint32_t *ctl = ictx(d, 0);
    ctl[0] = 0;
    ctl[1] = 1u | (1u << dci);

    ictx_copy_slot_from_output(d);
    uint32_t *sl = ictx(d, 1);
    sl[0] = (sl[0] & 0x07FFFFFFu) | ((uint32_t)dci << 27);

    uint32_t *ep = ictx(d, 1 + dci);
    ep_ctx_fill(ep, 7 /* Interrupt IN */, mps & 0x7FF, 0, &d->ring_in, mps & 0x7FF);
    ep[0] |= iv << 16;
    ep[4] |= (uint32_t)(mps & 0x7FF) << 16;                   /* Max ESIT Payload */

    int r = cmd_ok(phys_of(dev_mem[d->idx].inctx), 0, 0,
                   TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t)d->slot << 24), 0, 2000);
    if (r) return r;

    d->dci_in = dci; d->ep_in_addr = ep_addr; d->mps_in = mps;
    int c = xhci_control(d, 0x00, 9 /* SET_CONFIGURATION */, config_value, 0, 0, 0);
    return c < 0 ? c : 0;
}

int xhci_intr_submit(xhci_dev_t *d, void *buf, uint32_t len) {
    if (!d || !d->used || !x.up) return XHCI_E_NODEV;
    if (!d->dci_in || len == 0 || len > 1024) return XHCI_E_ARGS;
    uint32_t addr = phys_of(buf);
    if ((addr & 0xFFFFu) + len > 0x10000u) return XHCI_E_ARGS;     /* не перетинати 64 КБ */
    d->intr_done = 0;
    d->intr_len = len;
    d->intr_trb = ring_push(&d->ring_in, addr, 0, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP, 0);
    d->intr_armed = 1;
    ring_doorbell(d->slot, (uint32_t)d->dci_in);
    return 0;
}

int xhci_intr_poll(xhci_dev_t *d, uint32_t *actual) {
    if (!d || !d->used || !x.up) return XHCI_E_NODEV;
    if (!d->intr_armed) return 0;
    uint32_t e[4];
    for (int g = 0; g < 32 && !d->intr_done; g++) {
        if (!ev_poll(e)) break;
        ev_dispatch_other(e);
    }
    if (!d->intr_done) return 0;
    d->intr_armed = 0;
    d->intr_done = 0;
    d->last_cc = d->intr_cc;
    if (d->intr_cc == CC_SUCCESS || d->intr_cc == CC_SHORT) {
        if (actual) *actual = d->intr_resid <= d->intr_len ? d->intr_len - d->intr_resid : 0;
        return 1;
    }
    return cc_to_err(d->intr_cc);
}

int xhci_intr_recover(xhci_dev_t *d) {
    if (!d || !d->used || !x.up || !d->dci_in) return XHCI_E_NODEV;
    d->intr_armed = 0; d->intr_done = 0;
    return ep_recover(d, d->dci_in, &d->ring_in);
}

/* ---- список побачених пристроїв (для `usb list`) ---- */
static xhci_seen_t seen[XHCI_SEEN_MAX];
static int nseen;

int xhci_seen_count(void) { return nseen; }
const xhci_seen_t *xhci_seen_get(int i) { return (i >= 0 && i < nseen) ? &seen[i] : 0; }

const char *xhci_speed_str(int speed) {
    switch (speed) {
    case 1: return "USB 1.1 12Mbps";
    case 2: return "USB 1.0 1.5Mbps";
    case 3: return "USB 2.0 480Mbps";
    case 4: return "USB 3.0 5Gbps";
    case 5: return "USB 3.1+ 10Gbps";
    default: return "?";
    }
}

/* Рядковий дескриптор (UTF-16LE) -> ASCII; best effort, помилки мовчки ігноруються */
static void read_string(xhci_dev_t *d, uint8_t idx, char *out, int outsz) {
    out[0] = 0;
    if (!idx) return;
    uint8_t b[96];
    ctl_quiet = 1;
    int r = xhci_control(d, 0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, b, sizeof b);
    ctl_quiet = 0;
    if (r < 4 || b[1] != 3) return;
    int len = b[0] < r ? b[0] : r, n = 0;
    for (int i = 2; i + 1 < len && n < outsz - 1; i += 2) {
        uint16_t ch = (uint16_t)(b[i] | (b[i + 1] << 8));
        out[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static xhci_seen_t *seen_add(int port) {
    if (nseen >= XHCI_SEEN_MAX) return 0;
    xhci_seen_t *e = &seen[nseen++];
    zero(e, sizeof(*e));
    e->port = port;
    return e;
}

/* Один порт: reset, адреса, дескриптори, probe. Повертає 1, якщо пристрій залишено. */
static int enum_port(int port, xhci_probe_fn probe, void *arg) {
    xhci_seen_t *sn = seen_add(port);
    if (sn) sn->failed = 1;
    xhci_dev_t *d = dev_alloc();
    if (!d) { usb_printf("[xhci] port %d: too many devices\n", port); return 0; }
    d->port = port;

    int r = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        int speed = port_reset(port);
        if (speed < 0) {
            usb_printf("[xhci] port %d: reset failed (%d)\n", port, speed);
            dev_release(d);
            return 0;
        }
        d->speed = speed;
        if (sn) sn->speed = speed;
        USB_DBG("[xhci] port %d: speed %s, PORTSC=%08x\n", port, speed_name(speed), RD32(portsc_addr(port)));

        r = dev_address(d);
        if (r == 0) break;

        usb_printf("[xhci] port %d: address device failed (%d), %s\n", port, r, attempt ? "giving up" : "retrying");
        if (d->slot) {
            cmd_submit(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)d->slot << 24), 0, 0, 1000);
            dcbaa[d->slot] = 0;
            d->slot = 0;
        }
    }
    if (r) { dev_release(d); return 0; }

    /* Перші 8 байт device descriptor — щоб дізнатись реальний bMaxPacketSize0 */
    uint8_t dd[18];
    for (int i = 0; i < 18; i++) dd[i] = 0;
    r = xhci_control(d, 0x80, 6, 0x0100, 0, dd, 8);
    if (r < 8) {
        usb_printf("[xhci] port %d: cannot read device descriptor (%d)\n", port, r);
        dev_release(d);
        return 0;
    }
    uint32_t mps0 = (d->speed >= 4) ? (dd[7] <= 15 ? (1u << dd[7]) : 512u) : dd[7];
    if (mps0 == 0) mps0 = 8;
    if (mps0 != d->mps0) {
        USB_DBG("[xhci] port %d: EP0 max packet %u -> %u\n", port, d->mps0, mps0);
        if (dev_fix_mps0(d, mps0)) { dev_release(d); return 0; }
    }

    r = xhci_control(d, 0x80, 6, 0x0100, 0, dd, 18);
    if (r < 18) {
        usb_printf("[xhci] port %d: short device descriptor (%d)\n", port, r);
        dev_release(d);
        return 0;
    }
    d->dclass = dd[4]; d->dsub = dd[5]; d->dproto = dd[6];
    d->vid = (uint16_t)(dd[8] | (dd[9] << 8));
    d->pid = (uint16_t)(dd[10] | (dd[11] << 8));
    d->nconf = dd[17];
    if (sn) {
        sn->failed = 0; sn->vid = d->vid; sn->pid = d->pid;
        sn->dclass = d->dclass; sn->dsub = d->dsub; sn->dproto = d->dproto;
        read_string(d, dd[14], sn->manufacturer, sizeof sn->manufacturer);
        read_string(d, dd[15], sn->product, sizeof sn->product);
        if (d->dclass == 0 || d->dclass == 0xEF) {         /* клас задано в інтерфейсі: беремо перший */
            uint8_t cfg[128];
            ctl_quiet = 1;
            int cr = xhci_control(d, 0x80, 6, 0x0200, 0, cfg, sizeof cfg);
            ctl_quiet = 0;
            int len = cr < 9 ? 0 : (cfg[2] | (cfg[3] << 8));
            if (len > cr) len = cr;
            for (int o = cr >= 9 ? cfg[0] : 0; o + 9 <= len && cfg[o] >= 2; o += cfg[o])
                if (cfg[o + 1] == 4) { sn->dclass = cfg[o + 5]; sn->dsub = cfg[o + 6]; sn->dproto = cfg[o + 7]; break; }
        }
    }

    usb_printf("[xhci] port %d: %s device %04x:%04x class %02x\n", port, speed_name(d->speed), d->vid, d->pid, d->dclass);
    if (d->dclass == 9)
        usb_printf("[xhci] port %d: this is a USB hub - hubs are not supported yet, plug the drive directly into the laptop\n", port);

    if (probe && probe(d, arg)) { if (sn) sn->claimed = 1; return 1; }

    dev_release(d);
    return 0;
}

/* ================================================================== */
/*  Контролер                                                          */
/* ================================================================== */
static int wait_sts(uint32_t mask, uint32_t want, uint32_t ms) {
    usb_deadline_t dl = usb_deadline_ms(ms);
    for (;;) {
        if ((RD32(x.op + OP_USBSTS) & mask) == want) return 1;
        if (usb_expired(dl)) return 0;
        usb_delay_us(200);
    }
}

static void pci_enable(const pci_device_t *p) {
    /* Power management capability: якщо пристрій у D3 — повернути в D0 */
    uint16_t status = pci_config_read16(p->bus, p->slot, p->func, 0x06);
    if (status & (1u << 4)) {
        uint8_t ptr = (uint8_t)(pci_config_read32(p->bus, p->slot, p->func, 0x34) & 0xFC);
        for (int n = 0; ptr && n < 48; n++) {
            uint32_t hdr = pci_config_read32(p->bus, p->slot, p->func, ptr);
            if ((hdr & 0xFF) == 0x01) {
                uint16_t pmcsr = pci_config_read16(p->bus, p->slot, p->func, (uint8_t)(ptr + 4));
                if (pmcsr & 3) {
                    usb_printf("[xhci] controller was in D%u, switching to D0\n", pmcsr & 3);
                    pci_config_write16(p->bus, p->slot, p->func, (uint8_t)(ptr + 4), (uint16_t)(pmcsr & ~3u));
                    usb_delay_ms(20);
                }
                break;
            }
            ptr = (uint8_t)((hdr >> 8) & 0xFC);
        }
    }
    uint16_t cmd = pci_config_read16(p->bus, p->slot, p->func, 0x04);
    cmd |= 0x0002 | 0x0004;           /* Memory Space + Bus Master */
    cmd |= 0x0400;                    /* Interrupt Disable: працюємо полінгом */
    pci_config_write16(p->bus, p->slot, p->func, 0x04, cmd);
}

/* Intel PCH (Panther/Lynx Point): порти спільні з EHCI — треба перемкнути на xHCI. */
static void intel_route_ports(const pci_device_t *p) {
    if (p->vendor_id != 0x8086) return;
    switch (p->device_id) {
    case 0x1E31: case 0x8C31: case 0x9C31: case 0x9CB1: break;
    default: return;
    }
    uint32_t usb3 = pci_config_read32(p->bus, p->slot, p->func, 0xDC);   /* USB3_PSSEN capability */
    pci_config_write32(p->bus, p->slot, p->func, 0xD8, usb3);
    uint32_t usb2 = pci_config_read32(p->bus, p->slot, p->func, 0xD4);   /* XUSB2PRM */
    pci_config_write32(p->bus, p->slot, p->func, 0xD0, usb2);
    USB_DBG("[xhci] Intel port routing: usb3=%08x usb2=%08x\n", usb3, usb2);
}

/* Extended capabilities: BIOS handoff + список портів USB2/USB3. */
static void parse_ext_caps(void) {
    uint32_t xecp = (x.hcc1 >> 16) & 0xFFFF;
    if (!xecp) return;
    uintptr_t off = x.base + (uintptr_t)xecp * 4;

    for (int n = 0; n < 64; n++) {
        uint32_t hdr = RD32(off);
        uint32_t id = hdr & 0xFF;
        uint32_t next = (hdr >> 8) & 0xFF;

        if (id == 1) {                                   /* USB Legacy Support */
            if (hdr & (1u << 16)) {                      /* BIOS Owned Semaphore */
                usb_printf("[xhci] BIOS owns the controller, requesting handoff...\n");
                WR32(off, hdr | (1u << 24));             /* OS Owned Semaphore */
                usb_deadline_t dl = usb_deadline_ms(1000);
                while (!usb_expired(dl)) {
                    if (!(RD32(off) & (1u << 16))) break;
                    usb_delay_ms(1);
                }
                if (RD32(off) & (1u << 16)) {
                    usb_printf("[xhci] BIOS did not release - forcing\n");
                    WR32(off, (RD32(off) & ~(1u << 16)) | (1u << 24));
                }
            }
            /* вимкнути SMI від USB і скинути статуси (RW1C біти 29..31) */
            uint32_t ctl = RD32(off + 4);
            ctl &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15));
            ctl |= (7u << 29);
            WR32(off + 4, ctl);
        } else if (id == 2) {                            /* Supported Protocol */
            uint32_t major = hdr >> 24;
            uint32_t dw2 = RD32(off + 8);
            uint32_t first = dw2 & 0xFF, count = (dw2 >> 8) & 0xFF;
            for (uint32_t pnum = first; pnum < first + count && pnum < 256; pnum++)
                x.proto[pnum] = (uint8_t)major;
            USB_DBG("[xhci] protocol USB%u: ports %u..%u\n", major, first, first + count - 1);
        }

        if (!next) break;
        off += (uintptr_t)next * 4;
    }
}

static void ctrl_halt(void) {
    if (!x.base) return;
    uint32_t cmd = RD32(x.op + OP_USBCMD);
    if (cmd & CMD_RS) {
        WR32(x.op + OP_USBCMD, cmd & ~(CMD_RS | CMD_INTE));
        wait_sts(STS_HCH, STS_HCH, 500);
    }
    x.up = 0;
}

void xhci_shutdown(void) {
    ctrl_halt();
}

static int ctrl_init(const pci_device_t *p) {
    zero(&x, sizeof(x));
    x.pci = *p;

    uint32_t bar0 = p->bar0;
    if (bar0 & 1) { usb_printf("[xhci] BAR0 is an I/O port range - not an xHCI MMIO BAR\n"); return -10; }
    if ((bar0 & 6) == 4 && p->bar1 != 0) {
        usb_printf("[xhci] BAR0 is above 4 GB (%08x:%08x) - the 32-bit kernel cannot reach it\n", p->bar1, bar0 & ~0xFu);
        return -11;
    }
    x.base = (uintptr_t)(bar0 & ~0xFu);
    if (!x.base) { usb_printf("[xhci] BAR0 is not assigned by firmware\n"); return -12; }

    pci_enable(p);
    intel_route_ports(p);

    uint32_t cap0 = RD32(x.base);
    if (cap0 == 0xFFFFFFFFu || (cap0 & 0xFF) < 0x10) {
        usb_printf("[xhci] registers not readable (CAPLENGTH reg = %08x)\n", cap0);
        return -13;
    }
    uint32_t caplen = cap0 & 0xFF;
    x.version = cap0 >> 16;
    x.hcs1 = RD32(x.base + CAP_HCSPARAMS1);
    x.hcs2 = RD32(x.base + CAP_HCSPARAMS2);
    x.hcc1 = RD32(x.base + CAP_HCCPARAMS1);
    x.op   = x.base + caplen;
    x.rt   = x.base + (RD32(x.base + CAP_RTSOFF) & ~0x1Fu);
    x.db   = x.base + (RD32(x.base + CAP_DBOFF) & ~0x3u);

    x.max_slots = (int)(x.hcs1 & 0xFF);
    x.max_ports = (int)((x.hcs1 >> 24) & 0xFF);
    x.csz       = (x.hcc1 & (1u << 2)) ? 64 : 32;
    x.nscratch  = (int)((((x.hcs2 >> 21) & 0x1F) << 5) | ((x.hcs2 >> 27) & 0x1F));

    usb_printf("[xhci] %04x:%04x xHCI %x.%02x at %08x: %d ports, %d slots, ctx %d B, scratchpad %d\n",
               p->vendor_id, p->device_id, x.version >> 8, x.version & 0xFF, (uint32_t)x.base,
               x.max_ports, x.max_slots, x.csz, x.nscratch);

    if (x.max_ports == 0 || x.max_slots == 0) return -14;
    if (x.nscratch > MAX_SCRATCH) {
        usb_printf("[xhci] controller wants %d scratchpad pages (max %d)\n", x.nscratch, MAX_SCRATCH);
        return -15;
    }

    parse_ext_caps();

    /* --- зупинити і скинути --- */
    ctrl_halt();
    if (!wait_sts(STS_CNR, 0, 1000)) { usb_printf("[xhci] controller not ready (CNR) before reset\n"); return -16; }

    WR32(x.op + OP_USBCMD, CMD_HCRST);
    usb_delay_ms(5);
    usb_deadline_t dl = usb_deadline_ms(2000);
    while ((RD32(x.op + OP_USBCMD) & CMD_HCRST) && !usb_expired(dl)) usb_delay_ms(1);
    if (RD32(x.op + OP_USBCMD) & CMD_HCRST) { usb_printf("[xhci] HCRST did not complete\n"); return -17; }
    if (!wait_sts(STS_CNR, 0, 2000)) { usb_printf("[xhci] controller not ready (CNR) after reset\n"); return -18; }

    /* --- page size: беремо найменший підтримуваний; scratchpad-сторінки 4 КБ --- */
    uint32_t psz = RD32(x.op + OP_PAGESIZE) & 0xFFFF;
    if (!(psz & 1)) { usb_printf("[xhci] 4 KB pages not supported (PAGESIZE=%04x)\n", psz); return -19; }

    /* --- структури в пам'яті --- */
    zero(dcbaa, sizeof(dcbaa));
    zero(erst, sizeof(erst));
    zero(scratch_arr, sizeof(scratch_arr));
    if (x.nscratch) {
        for (int i = 0; i < x.nscratch; i++) {
            zero(scratch_pages[i], 4096);
            scratch_arr[i] = phys_of(scratch_pages[i]);
        }
        dcbaa[0] = phys_of(scratch_arr);
    }
    ring_init(&x.cmd, &cmd_ring_mem);
    zero(&evt_ring_mem, sizeof(evt_ring_mem));
    x.ev_phys  = phys_of(&evt_ring_mem);
    x.ev_deq   = 0;
    x.ev_cycle = 1;
    erst[0].base = x.ev_phys;
    erst[0].size = EV_TRBS;

    /* --- регістри --- */
    int slots = x.max_slots > 255 ? 255 : x.max_slots;
    WR32(x.op + OP_CONFIG, (uint32_t)slots);
    WR32(x.op + OP_DCBAAP,     phys_of(dcbaa));
    WR32(x.op + OP_DCBAAP + 4, 0);
    WR32(x.op + OP_CRCR,       x.cmd.phys | CRCR_RCS);
    WR32(x.op + OP_CRCR + 4,   0);

    WR32(x.rt + RT_ERSTSZ, 1);
    WR32(x.rt + RT_ERDP,       x.ev_phys);
    WR32(x.rt + RT_ERDP + 4,   0);
    WR32(x.rt + RT_ERSTBA,     phys_of(erst));            /* останнім: запис ініціалізує Event Ring */
    WR32(x.rt + RT_ERSTBA + 4, 0);
    WR32(x.rt + RT_IMAN, 1u);                             /* IP скинути, IE=0 — переривань не треба */
    WR32(x.rt + RT_IMOD, 0);

    WR32(x.op + OP_USBSTS, STS_HSE | STS_EINT | STS_PCD); /* RW1C */

    /* --- запуск --- */
    WR32(x.op + OP_USBCMD, CMD_RS);
    if (!wait_sts(STS_HCH, 0, 1000)) { usb_printf("[xhci] controller did not start (USBSTS=%08x)\n", RD32(x.op + OP_USBSTS)); return -20; }
    x.up = 1;

    /* --- живлення портів і чекання, поки лінки піднімуться (USB3 тренується до ~0.4 с) --- */
    for (int pn = 1; pn <= x.max_ports; pn++) {
        uint32_t v = RD32(portsc_addr(pn));
        if (!(v & PORTSC_PP)) WR32(portsc_addr(pn), portsc_neutral(v) | PORTSC_PP);
    }
    usb_delay_ms(300);
    usb_deadline_t settle = usb_deadline_ms(1200);
    while (!usb_expired(settle)) {
        int busy = 0;
        for (int pn = 1; pn <= x.max_ports; pn++) {
            uint32_t v = RD32(portsc_addr(pn));
            if (!(v & PORTSC_CCS) && PORTSC_PLS(v) == 7 /* Polling */) busy = 1;
        }
        if (!busy) break;
        usb_delay_ms(20);
    }
    return 0;
}

static int enumerate_all(xhci_probe_fn probe, void *arg) {
    int kept = 0;
    for (int pn = 1; pn <= x.max_ports; pn++) {
        uint32_t v = RD32(portsc_addr(pn));
        USB_DBG("[xhci] port %d: PORTSC=%08x (USB%u)\n", pn, v, x.proto[pn] ? x.proto[pn] : 2);
        if (v == 0xFFFFFFFFu || !(v & PORTSC_CCS)) continue;
        kept += enum_port(pn, probe, arg);
    }
    return kept;
}

int xhci_scan(xhci_probe_fn probe, void *arg) {
    usb_time_init();
    zero(devs, sizeof(devs));
    nseen = 0;

    pci_device_t p;
    int found = 0, ok = 0;
    for (int ci = 0; ci < 4; ci++) {
        if (!pci_find_class_n(0x0C, 0x03, 0x30, ci, &p)) break;
        found++;
        int r = ctrl_init(&p);
        if (r != 0) {
            usb_printf("[xhci] controller %d init failed (%d)\n", ci, r);
            ctrl_halt();
            continue;
        }
        ok++;
        int n = enumerate_all(probe, arg);
        if (n > 0) return n;
        ctrl_halt();
    }
    if (!found) {
        usb_printf("[xhci] no xHCI controller on the PCI bus\n");
        return XHCI_E_NODEV;
    }
    return ok ? 0 : XHCI_E_NODEV;
}

void xhci_print_info(void) {
    if (!x.base) { usb_printf("xHCI: not initialised (run 'usb')\n"); return; }
    usb_printf("xHCI %04x:%04x v%x.%02x BAR0=%08x ports=%d slots=%d ctx=%dB %s\n",
               x.pci.vendor_id, x.pci.device_id, x.version >> 8, x.version & 0xFF, (uint32_t)x.base,
               x.max_ports, x.max_slots, x.csz, x.up ? "running" : "stopped");
    if (!x.up) return;
    for (int pn = 1; pn <= x.max_ports; pn++) {
        uint32_t v = RD32(portsc_addr(pn));
        if (v & PORTSC_CCS)
            usb_printf("  port %d (USB%u): connected, %s%s PORTSC=%08x\n", pn, x.proto[pn] ? x.proto[pn] : 2,
                       speed_name((int)PORTSC_SPEED(v)), (v & PORTSC_PED) ? "" : " (disabled)", v);
    }
    for (int i = 0; i < XHCI_MAX_DEVS; i++)
        if (devs[i].used)
            usb_printf("  device slot %d on port %d: %04x:%04x\n", devs[i].slot, devs[i].port, devs[i].vid, devs[i].pid);
}
