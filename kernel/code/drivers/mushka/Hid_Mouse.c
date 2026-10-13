#include "../../../headers/head_mouse.h"
#include "../../../headers/usb_mem.h"
#include "../../../headers/PCI.h"
#include "../../../../standarts_lib/stdio.h"
#include <stdint.h>

static void mouse_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t*)ptr;
    for (uint32_t i = 0; i < size; i++) p[i] = 0;
}

/* Шукаємо інтерфейс HID / Boot / Mouse (class 3, subclass 1, protocol 2)
 * та його interrupt-IN endpoint. Клавіатура (protocol 1) більше не підходить. */
static int parse_config_for_hid_mouse(const uint8_t *buf, uint16_t total_len,
                                      uint8_t *out_ep, uint16_t *out_mps,
                                      uint8_t *out_iface, uint8_t *out_interval) {
    uint16_t off = 0;
    int in_mouse = 0;

    while (off + 2 <= total_len) {
        uint8_t len  = buf[off];
        uint8_t type = buf[off + 1];
        if (len < 2 || off + len > total_len) break;

        if (type == 0x04 && len >= 9) {                 /* INTERFACE */
            uint8_t alt   = buf[off + 3];
            uint8_t cls   = buf[off + 5];
            uint8_t sub   = buf[off + 6];
            uint8_t proto = buf[off + 7];
            in_mouse = (alt == 0 && cls == 0x03 && sub == 0x01 && proto == 0x02);
            if (in_mouse) *out_iface = buf[off + 2];
        } else if (type == 0x05 && in_mouse && len >= 7) {   /* ENDPOINT */
            uint8_t addr = buf[off + 2];
            uint8_t attr = buf[off + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {    /* IN + interrupt */
                *out_ep = addr & 0x0F;
                *out_mps = (uint16_t)((buf[off + 4] | (buf[off + 5] << 8)) & 0x7FF);
                *out_interval = buf[off + 6];
                return 1;
            }
        }
        off = (uint16_t)(off + len);
    }
    return 0;
}

static int hid_set_idle(hid_mouse_t *m, uint8_t iface) {
    return usb_control_transfer(&m->hc, &m->dev, 0x21, HID_REQ_SET_IDLE, 0, iface, 0, 0, 0);
}

static int hid_set_protocol_boot(hid_mouse_t *m, uint8_t iface) {
    return usb_control_transfer(&m->hc, &m->dev, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, iface, 0, 0, 0);
}

/* Спроба зробити мишку з пристрою на порту. 1 — вийшло. */
static int try_port(hid_mouse_t *m, int port) {
    uhci_controller_t *hc = &m->hc;

    t_printf("[mouse] port %d: device connected, reset...\n", port);
    if (!uhci_port_reset(hc, port)) {
        t_printf("[mouse] port %d: reset failed\n", port);
        return 0;
    }

    m->dev.address = 0;
    m->dev.max_packet_size0 = 8;                       /* безпечний мінімум до першого дескриптора */
    m->dev.low_speed = uhci_port_is_lowspeed(hc, port) ? 1 : 0;
    t_printf("[mouse] port %d: %s speed\n", port, m->dev.low_speed ? "low" : "full");

    /* 1) Спершу лише 8 байт дескриптора, з них байт 7 = bMaxPacketSize0.
     *    Читати одразу 18 байт кусками по 8 не можна для full-speed пристроїв з mps0 = 64. */
    uint8_t head[8];
    if (usb_get_descriptor(hc, &m->dev, USB_DESC_DEVICE, 0, 8, head) != 0) {
        t_printf("[mouse] port %d: GET_DESCRIPTOR(device,8) failed\n", port);
        goto fail;
    }
    uint8_t mps0 = head[7];
    if (m->dev.low_speed || mps0 < 8 || mps0 > 64) mps0 = 8;
    m->dev.max_packet_size0 = mps0;

    if (head[4] == 0x09) {                             /* bDeviceClass = Hub */
        t_printf("[mouse] port %d: this is a hub, skipping\n", port);
        goto fail;
    }

    /* 2) Адреса. Пристрій один — беремо 1. Порт мусить лишатись увімкненим. */
    if (usb_set_address(hc, &m->dev, 1) != 0) {
        t_printf("[mouse] port %d: SET_ADDRESS failed\n", port);
        goto fail;
    }

    usb_device_descriptor_t dd;
    if (usb_get_device_descriptor(hc, &m->dev, &dd) == 0) {
        t_printf("[mouse] port %d: VID=%d PID=%d class=%d\n", port, dd.idVendor, dd.idProduct, dd.bDeviceClass);
    }

    /* 3) Конфіг: спочатку 9 байт -> wTotalLength, потім увесь */
    uint8_t cfg_head[9];
    if (usb_get_descriptor(hc, &m->dev, USB_DESC_CONFIG, 0, 9, cfg_head) != 0) {
        t_printf("[mouse] port %d: GET_DESCRIPTOR(config,9) failed\n", port);
        goto fail;
    }
    uint16_t total_len = (uint16_t)(cfg_head[2] | (cfg_head[3] << 8));
    if (total_len < 9) total_len = 9;
    if (total_len > 256) total_len = 256;

    uint8_t cfg[256];
    if (usb_get_descriptor(hc, &m->dev, USB_DESC_CONFIG, 0, total_len, cfg) != 0) {
        t_printf("[mouse] port %d: GET_DESCRIPTOR(config,%d) failed\n", port, total_len);
        goto fail;
    }

    uint8_t ep = 0, iface = 0, interval = 10;
    uint16_t mps = 8;
    if (!parse_config_for_hid_mouse(cfg, total_len, &ep, &mps, &iface, &interval)) {
        t_printf("[mouse] port %d: not a boot-protocol mouse (keyboard/other), skipping\n", port);
        goto fail;
    }
    if (mps == 0 || mps > 64) mps = 8;
    if (m->dev.low_speed && mps > 8) mps = 8;
    t_printf("[mouse] port %d: HID mouse endpoint=%d mps=%d interval=%d\n", port, ep, mps, interval);

    /* 4) Конфігурація, потім boot protocol (SET_IDLE пристрій має право відхилити — це не помилка) */
    if (usb_set_configuration(hc, &m->dev, cfg[5]) != 0) {
        t_printf("[mouse] port %d: SET_CONFIGURATION failed\n", port);
        goto fail;
    }
    hid_set_idle(m, iface);
    if (hid_set_protocol_boot(m, iface) != 0) {
        t_printf("[mouse] port %d: SET_PROTOCOL(boot) rejected, continuing\n", port);
    }

    m->endpoint_addr = ep;
    m->max_packet = mps;
    m->interface_num = iface;
    m->interval = interval;
    m->port = (int8_t)port;
    m->toggle = 0;
    m->dead = 0;
    m->errors = 0;

    /* 5) Запускаємо постійну interrupt-чергу. Далі hid_mouse_poll лише зчитує результат. */
    uhci_int_start(hc, m->dev.address, m->dev.low_speed, ep, mps, interval);
    return 1;

fail:
    uhci_port_disable(hc, port);     /* щоб "чужий" пристрій з адресою 1 не заважав наступним */
    return 0;
}

int hid_mouse_init(hid_mouse_t *m) {
    mouse_zero(m, sizeof(*m));
    m->port = -1;

    int seen = 0;
    for (int idx = 0; idx < 8; idx++) {
        pci_device_t pdev;
        if (!pci_find_uhci_n(idx, &pdev)) break;
        seen++;

        t_printf("[mouse] UHCI #%d: bus=%d slot=%d func=%d io=%d irq=%d\n",
                 idx, pdev.bus, pdev.slot, pdev.func, pdev.bar4 & 0xFFFC, pdev.irq_line);

        if (!uhci_init(&m->hc, &pdev)) {
            t_printf("[mouse] UHCI #%d: init failed\n", idx);
            uhci_stop(&m->hc);
            continue;
        }

        for (int port = 0; port < m->hc.num_ports; port++) {
            if (!uhci_port_connected(&m->hc, port)) continue;
            if (try_port(m, port)) {
                t_printf("[mouse] USB mouse ready (UHCI #%d, port %d)\n", idx, port);
                return 1;
            }
        }
        uhci_stop(&m->hc);          /* тут мишки немає — не тримаємо контролер запущеним */
    }

    if (!seen)
        t_printf("[mouse] no UHCI controller on PCI bus (VM/PC may have EHCI/xHCI only)\n");
    else
        t_printf("[mouse] UHCI present, but no USB mouse on its ports\n");
    return 0;
}

int hid_mouse_poll(hid_mouse_t *m, mouse_report_t *out) {
    if (m->dead) return 0;

    uint8_t buf[64];
    uint8_t len = 0;
    int r = uhci_int_poll(&m->hc, buf, &len);

    if (r < 0) {
        if (++m->errors >= 64) {                 /* багато підряд помилок — може, висмикнули? */
            m->errors = 0;
            if (!uhci_port_connected(&m->hc, m->port)) {
                m->dead = 1;
                uhci_int_stop(&m->hc);
            }
        }
        return 0;
    }
    if (r == 0) return 0;

    m->errors = 0;
    if (len < 3) return 0;

    out->buttons = buf[0] & 0x07;
    out->dx      = (int8_t)buf[1];
    out->dy      = (int8_t)buf[2];
    out->wheel   = (len >= 4) ? (int8_t)buf[3] : 0;
    return 1;
}