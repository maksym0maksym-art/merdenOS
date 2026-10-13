#include "../../../headers/uhci.h"
#include "../../../headers/usb_mem.h"
#include "../../../headers/ports.h"

/*
 * Уся DMA-пам'ять UHCI — одна статична структура в .bss (вирівняна на 4 КБ).
 * Жодних usb_alloc() під час роботи: раніше кожен poll мишки "з'їдав" 48 байт
 * пулу, і за секунду-дві пул закінчувався (usb_alloc повертав NULL -> запис в адресу 0).
 */
#define UHCI_MAX_CTL_TDS 40
#define UHCI_CTL_BUF     512

typedef struct {
    uint32_t  frame_list[1024];          /* має бути вирівняно на 4096 */
    uhci_qh_t qh_int;                    /* періодична черга (HID) */
    uhci_qh_t qh_ctl;                    /* control-черга */
    uhci_td_t int_td;
    uhci_td_t ctl_td[UHCI_MAX_CTL_TDS];
    uint8_t   setup[16];
    uint8_t   ctl_buf[UHCI_CTL_BUF];
    uint8_t   int_buf[64];
} __attribute__((aligned(4096))) uhci_dma_t;

static uhci_dma_t dma;

/* стан interrupt-endpoint'а */
static struct {
    int      active;
    uint8_t  addr, ep, low_speed, toggle;
    uint16_t maxlen;
} intr;

static inline void barrier(void) { __asm__ volatile ("" ::: "memory"); }

static void zero_mem(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}

/* ------------------------------------------------------------------ */
/*  Затримки.                                                          */
/*  Стара delay_ms крутила volatile-цикл на 20000 ітерацій — це десь    */
/*  30 мкс, а не 1 мс. USB вимагає 50 мс на reset порту, тому пристрій  */
/*  не встигав отямитись. Тут — реальні мілісекунди по PIT (канал 2).   */
/* ------------------------------------------------------------------ */
static void pit_wait_1ms(void) {
    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));   /* gate2 ON, динамік OFF */
    outb(0x43, 0xB0);                                   /* ch2, lo/hi, mode 0 */
    outb(0x42, 1193 & 0xFF);                            /* 1193182 Гц / 1000 */
    outb(0x42, 1193 >> 8);
    for (uint32_t i = 0; i < 400000; i++) {             /* страховка, якщо PIT "мовчить" */
        if (inb(0x61) & 0x20) break;
    }
}

void uhci_delay_ms(uint32_t ms) {
    while (ms--) pit_wait_1ms();
}

/* ------------------------------------------------------------------ */
/*  Контролер                                                          */
/* ------------------------------------------------------------------ */
static int wait_halted(uhci_controller_t *hc, int want_halted, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        int halted = (inw(hc->io_base + UHCI_USBSTS) & UHCI_STS_HCHALTED) != 0;
        if (halted == want_halted) return 1;
        uhci_delay_ms(1);
    }
    return 0;
}

int uhci_init(uhci_controller_t *hc, const pci_device_t *pdev) {
    hc->io_base   = (uint16_t)(pdev->bar4 & 0xFFFC);
    hc->irq_line  = pdev->irq_line;
    hc->pci_bus   = pdev->bus;
    hc->pci_slot  = pdev->slot;
    hc->pci_func  = pdev->func;
    hc->num_ports = 0;
    if (hc->io_base == 0) return 0;

    uint16_t io = hc->io_base;

    /* 1) PCI: I/O space + Bus Master. БЕЗ Bus Master контролер не читає TD з RAM
     *    і жодна транзакція не завершується (це і був "timeout -2"). */
    uint16_t cmd = pci_config_read16(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04);
    pci_config_write16(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04, (uint16_t)(cmd | 0x0005));

    /* 2) забрати контролер у BIOS (legacy USB -> PS/2 емуляція) */
    pci_config_write16(hc->pci_bus, hc->pci_slot, hc->pci_func, UHCI_PCI_LEGSUP, 0x8F00);

    /* 3) зупинити, глобальний reset шини, reset контролера */
    outw(io + UHCI_USBCMD, 0);
    wait_halted(hc, 1, 20);

    outw(io + UHCI_USBCMD, UHCI_CMD_GRESET);
    uhci_delay_ms(50);
    outw(io + UHCI_USBCMD, 0);
    uhci_delay_ms(10);

    outw(io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if ((inw(io + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0) break;
        uhci_delay_ms(1);
    }
    outw(io + UHCI_USBINTR, 0);                 /* працюємо полінгом, переривання не потрібні */
    outw(io + UHCI_USBSTS, UHCI_STS_ALL);

    pci_config_write16(hc->pci_bus, hc->pci_slot, hc->pci_func, UHCI_PCI_LEGSUP, 0x2000);

    /* 4) скільки в контролера портів (біт 7 PORTSC завжди 1 у реального порту) */
    for (int p = 0; p < 8; p++) {
        uint16_t v = inw(io + UHCI_PORTSC1 + p * 2);
        if (v == 0xFFFF || !(v & 0x0080)) break;
        hc->num_ports++;
    }

    /* 5) розклад: у КОЖНОМУ кадрі виконується наша черга.
     *    Раніше QH стояв лише в frame_list[0], тобто виконувався 1 раз на 1024 мс —
     *    звідси таймаути на 20 мс у poll. */
    zero_mem(&dma, sizeof(dma));
    intr.active = 0;

    dma.qh_ctl.head_link    = UHCI_PTR_TERMINATE;
    dma.qh_ctl.element_link = UHCI_PTR_TERMINATE;
    dma.qh_int.head_link    = usb_virt_to_phys(&dma.qh_ctl) | UHCI_PTR_QH;
    dma.qh_int.element_link = UHCI_PTR_TERMINATE;
    for (int i = 0; i < 1024; i++)
        dma.frame_list[i] = usb_virt_to_phys(&dma.qh_ctl) | UHCI_PTR_QH;
    barrier();

    outl(io + UHCI_FRBASEADD, usb_virt_to_phys(dma.frame_list));
    outw(io + UHCI_FRNUM, 0);
    outb(io + UHCI_SOFMOD, 64);
    outw(io + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    if (!wait_halted(hc, 0, 20)) return 0;      /* контролер не запустився */
    return 1;
}

void uhci_stop(uhci_controller_t *hc) {
    if (!hc->io_base) return;
    intr.active = 0;
    outw(hc->io_base + UHCI_USBCMD, 0);
    wait_halted(hc, 1, 20);
}

/* ------------------------------------------------------------------ */
/*  Порти                                                              */
/* ------------------------------------------------------------------ */
static uint16_t portsc_reg(uhci_controller_t *hc, int port) {
    return (uint16_t)(hc->io_base + UHCI_PORTSC1 + port * 2);
}

static void portsc_set(uhci_controller_t *hc, int port, uint16_t bits) {
    uint16_t r = portsc_reg(hc, port);
    outw(r, (uint16_t)((inw(r) & ~UHCI_PORTSC_RWC) | bits));
}

static void portsc_clear(uhci_controller_t *hc, int port, uint16_t bits) {
    uint16_t r = portsc_reg(hc, port);
    outw(r, (uint16_t)(inw(r) & ~UHCI_PORTSC_RWC & ~bits));
}

static void portsc_ack_changes(uhci_controller_t *hc, int port) {
    uint16_t r = portsc_reg(hc, port);
    outw(r, (uint16_t)(inw(r) | UHCI_PORTSC_RWC));
}

int uhci_port_connected(uhci_controller_t *hc, int port) {
    if (port < 0 || port >= hc->num_ports) return 0;
    return (inw(portsc_reg(hc, port)) & UHCI_PORTSC_CONNECT) != 0;
}

int uhci_port_is_lowspeed(uhci_controller_t *hc, int port) {
    if (port < 0 || port >= hc->num_ports) return 0;
    return (inw(portsc_reg(hc, port)) & UHCI_PORTSC_LOWSPEED) != 0;
}

int uhci_port_reset(uhci_controller_t *hc, int port) {
    if (port < 0 || port >= hc->num_ports) return 0;

    portsc_set(hc, port, UHCI_PORTSC_RESET);
    uhci_delay_ms(50);                       /* TDRST: >= 50 мс */
    portsc_clear(hc, port, UHCI_PORTSC_RESET);
    uhci_delay_ms(10);                       /* TRSTRCY: >= 10 мс на "отямитись" */

    for (int i = 0; i < 100; i++) {
        uint16_t v = inw(portsc_reg(hc, port));
        if (!(v & UHCI_PORTSC_CONNECT)) return 0;           /* пристрій зник */
        if (v & UHCI_PORTSC_ENABLE) {
            portsc_ack_changes(hc, port);
            uhci_delay_ms(10);
            return 1;
        }
        portsc_set(hc, port, UHCI_PORTSC_ENABLE);
        uhci_delay_ms(2);
    }
    return 0;
}

void uhci_port_disable(uhci_controller_t *hc, int port) {
    if (port < 0 || port >= hc->num_ports) return;
    portsc_clear(hc, port, UHCI_PORTSC_ENABLE);
    portsc_ack_changes(hc, port);
}

/* ------------------------------------------------------------------ */
/*  Control-транзакція                                                 */
/* ------------------------------------------------------------------ */
static uint32_t make_token(uint16_t len, uint8_t endpoint, uint8_t addr, uint8_t pid, uint8_t toggle) {
    uint32_t maxlen_bits = (len == 0) ? 0x7FFu : (uint32_t)(len - 1);
    return ((maxlen_bits & 0x7FFu) << 21) |
           ((uint32_t)(toggle & 1) << 19) |
           ((uint32_t)(endpoint & 0xF) << 15) |
           ((uint32_t)(addr & 0x7F) << 8) |
           (uint32_t)pid;
}

int uhci_control_transfer(uhci_controller_t *hc, uint8_t addr, int low_speed, uint16_t mps0,
                          const uint8_t setup[8], void *data, uint16_t len, int dir_in) {
    (void)hc;
    if (mps0 < 8) mps0 = 8;
    if (len > UHCI_CTL_BUF) return UHCI_ERR_ARGS;

    uint32_t ls = low_speed ? TD_CTRL_LS : 0;
    /* ВАЖЛИВО: Active — це біт 7 СТАТУСУ, тобто біт 23 слова, тож його треба зсунути на
     * TD_CTRL_STATUS_SHIFT. Раніше в різних місцях перевіряли "ctrl_status & TD_STATUS_ACTIVE"
     * без зсуву — це біт 7 поля actual length, і на нульових пакетах (STATUS-стадія)
     * воно завжди було 1, тому SET_ADDRESS "провалювався" з -3. */
    uint32_t base = (TD_STATUS_ACTIVE << TD_CTRL_STATUS_SHIFT) | TD_CTRL_C_ERR3 | ls;

    for (int i = 0; i < 8; i++) dma.setup[i] = setup[i];
    if (!dir_in && len) {
        uint8_t *src = (uint8_t *)data;
        for (uint16_t i = 0; i < len; i++) dma.ctl_buf[i] = src[i];
    }

    int n = 0;

    /* SETUP (DATA0) */
    uhci_td_t *td = &dma.ctl_td[n++];
    td->token       = make_token(8, 0, addr, TD_PID_SETUP, 0);
    td->buffer      = usb_virt_to_phys(dma.setup);
    td->ctrl_status = base;

    /* DATA (DATA1, далі чергуємо) */
    uint8_t toggle = 1;
    uint16_t off = 0;
    while (off < len) {
        if (n >= UHCI_MAX_CTL_TDS - 1) return UHCI_ERR_ARGS;
        uint16_t chunk = (uint16_t)((len - off) > mps0 ? mps0 : (len - off));
        td = &dma.ctl_td[n++];
        td->token       = make_token(chunk, 0, addr, dir_in ? TD_PID_IN : TD_PID_OUT, toggle);
        td->buffer      = usb_virt_to_phys(&dma.ctl_buf[off]);
        td->ctrl_status = base;
        off = (uint16_t)(off + chunk);
        toggle ^= 1;
    }

    /* STATUS: протилежний напрямок, завжди DATA1, нуль байт */
    td = &dma.ctl_td[n++];
    td->token       = make_token(0, 0, addr, (len == 0 || !dir_in) ? TD_PID_IN : TD_PID_OUT, 1);
    td->buffer      = 0;
    td->ctrl_status = base;

    for (int i = 0; i < n - 1; i++)
        dma.ctl_td[i].link = usb_virt_to_phys(&dma.ctl_td[i + 1]) | UHCI_PTR_DEPTH;
    dma.ctl_td[n - 1].link = UHCI_PTR_TERMINATE;

    barrier();
    dma.qh_ctl.element_link = usb_virt_to_phys(&dma.ctl_td[0]);   /* старт */

    int rc = UHCI_ERR_TIME;
    for (uint32_t t = 0; t < 750; t++) {
        int all_done = 1;
        for (int i = 0; i < n; i++) {
            uint32_t st = (dma.ctl_td[i].ctrl_status >> TD_CTRL_STATUS_SHIFT) & 0xFF;
            if (st & TD_STATUS_ACTIVE) { all_done = 0; break; }
            if (st & TD_STATUS_STALLED) { rc = UHCI_ERR_STALL; goto finish; }
            if (st & TD_STATUS_ERRORS)  { rc = UHCI_ERR_XFER;  goto finish; }
        }
        if (all_done) { rc = UHCI_OK; break; }
        uhci_delay_ms(1);
    }

finish:
    dma.qh_ctl.element_link = UHCI_PTR_TERMINATE;
    barrier();
    uhci_delay_ms(2);                          /* дати контролеру покинути TD */

    if (rc == UHCI_OK && dir_in && len) {
        uint8_t *dst = (uint8_t *)data;
        for (uint16_t i = 0; i < len; i++) dst[i] = dma.ctl_buf[i];
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Interrupt-IN (мишка)                                               */
/* ------------------------------------------------------------------ */
static void int_arm(void) {
    uhci_td_t *td = &dma.int_td;
    td->link   = UHCI_PTR_TERMINATE;
    td->token  = make_token(intr.maxlen, intr.ep, intr.addr, TD_PID_IN, intr.toggle);
    td->buffer = usb_virt_to_phys(dma.int_buf);
    barrier();
    /* Active виставляємо ОСТАННІМ */
    td->ctrl_status = (TD_STATUS_ACTIVE << TD_CTRL_STATUS_SHIFT) | TD_CTRL_C_ERR3 |
                      (intr.low_speed ? TD_CTRL_LS : 0);
    barrier();
    dma.qh_int.element_link = usb_virt_to_phys(td);
}

int uhci_int_start(uhci_controller_t *hc, uint8_t addr, int low_speed,
                   uint8_t ep, uint16_t maxlen, uint8_t interval) {
    (void)hc;
    if (maxlen == 0 || maxlen > sizeof(dma.int_buf)) maxlen = 8;

    intr.addr = addr;
    intr.ep = ep;
    intr.low_speed = low_speed ? 1 : 0;
    intr.maxlen = maxlen;
    intr.toggle = 0;
    int_arm();
    intr.active = 1;

    /* період опитування = найбільший степінь двійки <= bInterval (мс), 1..32 */
    uint32_t period = 1;
    while (period * 2 <= interval && period < 32) period *= 2;

    for (uint32_t i = 0; i < 1024; i++) {
        dma.frame_list[i] = (i % period == 0)
            ? (usb_virt_to_phys(&dma.qh_int) | UHCI_PTR_QH)
            : (usb_virt_to_phys(&dma.qh_ctl) | UHCI_PTR_QH);
    }
    barrier();
    return 1;
}

int uhci_int_poll(uhci_controller_t *hc, uint8_t *out, uint8_t *len) {
    (void)hc;
    if (!intr.active) return 0;

    uhci_td_t *td = &dma.int_td;
    uint32_t cs = td->ctrl_status;
    uint32_t st = (cs >> TD_CTRL_STATUS_SHIFT) & 0xFF;

    if (st & TD_STATUS_ACTIVE) {
        /* Пристрій ще NAK-ає ("руху немає") — це норма. Страховка: якщо контролер
         * після попереднього пакета затер element_link, повертаємо його. */
        if (dma.qh_int.element_link != usb_virt_to_phys(td))
            dma.qh_int.element_link = usb_virt_to_phys(td);
        return 0;
    }

    int result;
    if (st & TD_STATUS_ERRORS) {
        result = UHCI_ERR_XFER;                       /* toggle не чіпаємо */
    } else {
        uint32_t n = (cs + 1) & TD_CTRL_ACTLEN_MASK;  /* ActLen кодується як n-1 (0x7FF = 0 байт) */
        if (n > intr.maxlen) n = intr.maxlen;
        for (uint32_t i = 0; i < n; i++) out[i] = dma.int_buf[i];
        *len = (uint8_t)n;
        intr.toggle ^= 1;
        result = 1;
    }

    int_arm();
    return result;
}

void uhci_int_stop(uhci_controller_t *hc) {
    (void)hc;
    intr.active = 0;
    dma.qh_int.element_link = UHCI_PTR_TERMINATE;
    barrier();
}
