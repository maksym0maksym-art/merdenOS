#include <stdint.h>
#include "../../../headers/xhci_hid.h"
#include "../../../headers/usb_dbg.h"
#include "../../../headers/usb_msc.h"

#define HID_MAX 2

typedef struct {
    xhci_dev_t *d;
    uint8_t     iface;
    uint16_t    mps;
    uint32_t    len;
    int         dead;
    int         errs;
} hid_t;

static hid_t   mice[HID_MAX];
static int     nmice;
static uint8_t rbuf[HID_MAX][16] __attribute__((aligned(64)));

void xhci_hid_forget_all(void) {
    for (int i = 0; i < HID_MAX; i++) { mice[i].d = 0; mice[i].dead = 0; mice[i].errs = 0; }
    nmice = 0;
}

int xhci_mouse_count(void) { return nmice; }

static int find_mouse(const uint8_t *c, int len, uint8_t *iface, uint8_t *ep, uint16_t *mps, uint8_t *interval) {
    int o = 0, cur = 0, found_if = -1;
    while (o + 2 <= len && c[o] >= 2 && o + c[o] <= len) {
        if (c[o + 1] == 4 && c[o] >= 9) {                              /* interface */
            cur = c[o + 2];
            found_if = (c[o + 5] == 3 && c[o + 6] == 1 && c[o + 7] == 2 && c[o + 3] == 0) ? cur : -1;
        } else if (c[o + 1] == 5 && c[o] >= 7 && found_if >= 0) {     /* endpoint */
            if ((c[o + 2] & 0x80) && (c[o + 3] & 3) == 3) {
                *iface = (uint8_t)found_if; *ep = c[o + 2];
                *mps = (uint16_t)((c[o + 4] | (c[o + 5] << 8)) & 0x7FF);
                *interval = c[o + 6];
                return 1;
            }
        }
        o += c[o];
    }
    return 0;
}

static int arm(int i) {
    hid_t *h = &mice[i];
    return xhci_intr_submit(h->d, rbuf[i], h->len);
}

int xhci_hid_probe(xhci_dev_t *d, void *arg) {
    (void)arg;
    if (nmice >= HID_MAX) return 0;
    if (d->dclass != 0 && d->dclass != 3 && d->dclass != 0xEF) return 0;

    uint8_t cfg[256];
    int r = xhci_control(d, 0x80, 6, 0x0200, 0, cfg, 9);
    if (r < 9) return 0;
    int total = cfg[2] | (cfg[3] << 8);
    if (total > (int)sizeof cfg) total = (int)sizeof cfg;
    r = xhci_control(d, 0x80, 6, 0x0200, 0, cfg, (uint16_t)total);
    if (r < 9) return 0;

    uint8_t iface, ep, interval; uint16_t mps;
    if (!find_mouse(cfg, r, &iface, &ep, &mps, &interval)) return 0;
    if (mps < 3) return 0;

    usb_printf("[hid] boot mouse: iface %u, IN %02x, mps %u, interval %u\n", iface, ep, mps, interval);

    if (xhci_configure_intr_in(d, cfg[5], ep, mps, interval) != 0) {
        usb_printf("[hid] cannot configure interrupt endpoint\n");
        return 0;
    }
    /* SET_PROTOCOL(boot) і SET_IDLE(0): помилки (STALL) не критичні */
    xhci_control(d, 0x21, 0x0B, 0, iface, 0, 0);
    xhci_control(d, 0x21, 0x0A, 0, iface, 0, 0);

    int i = nmice++;
    mice[i].d = d; mice[i].iface = iface; mice[i].mps = mps;
    mice[i].len = mps > sizeof rbuf[0] ? (uint32_t)sizeof rbuf[0] : mps;
    mice[i].dead = 0; mice[i].errs = 0;
    if (arm(i) != 0) { mice[i].dead = 1; usb_printf("[hid] cannot start polling\n"); }
    return 1;
}

/* Один скан забирає і USB-миші, і флешки (повторний скан скидає контролер і псує вже взяті пристрої) */
static int probe_msc_hid(xhci_dev_t *d, void *arg) {
    if (usb_msc_probe(d, arg)) return 1;
    return xhci_hid_probe(d, arg);
}

int xhci_mouse_init(void) {
    xhci_hid_forget_all();
    usb_msc_forget_all();
    int n = xhci_scan(probe_msc_hid, 0);
    if (n < 0 || nmice == 0) { nmice = 0; return 0; }
    usb_printf("[hid] %d USB mouse(s) via xHCI\n", nmice);
    return nmice;
}

int xhci_mouse_read(int *dx, int *dy, int *wheel, uint8_t *buttons) {
    for (int i = 0; i < nmice; i++) {
        hid_t *h = &mice[i];
        if (h->dead || !h->d) continue;
        uint32_t got = 0;
        int r = xhci_intr_poll(h->d, &got);
        if (r == 0) continue;
        if (r < 0) {
            if (++h->errs > 20) { h->dead = 1; usb_printf("[hid] mouse %d stopped (too many errors)\n", i); continue; }
            xhci_intr_recover(h->d);
            arm(i);
            continue;
        }
        h->errs = 0;
        const uint8_t *b = rbuf[i];
        int ok = got >= 3;
        if (ok) {
            *buttons = b[0] & 7;
            *dx = (int8_t)b[1];
            *dy = (int8_t)b[2];
            *wheel = got >= 4 ? (int8_t)b[3] : 0;
        }
        arm(i);
        if (ok) return 1;
    }
    return 0;
}
