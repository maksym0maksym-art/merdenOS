#include "../../../headers/head_mouse.h"
#include "../../../headers/usb_mem.h"
#include "../../../headers/pci.h"
#include "../../../../standarts_lib/stdio.h"

static void mouse_zero(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t*)ptr;
    for (uint32_t i = 0; i < size; i++) p[i] = 0;
}

static int parse_config_for_hid_endpoint(uint8_t *buf, uint16_t total_len,
                                          uint8_t *out_ep_addr, uint16_t *out_mps,
                                          uint8_t *out_interface_num) {
    uint16_t offset = 0;
    int in_target_interface = 0;
 
    while (offset + 2 <= total_len) {
        uint8_t len = buf[offset];
        uint8_t type = buf[offset + 1];
        if (len == 0) break;
 
        if (type == 0x04) {
            uint8_t iface_num = buf[offset + 2];
            uint8_t iface_class = buf[offset + 5];
            in_target_interface = (iface_class == 0x03);
            if (in_target_interface) *out_interface_num = iface_num;
        } else if (type == 0x05  && in_target_interface) {
            usb_endpoint_descriptor_t *ep = (usb_endpoint_descriptor_t*)(buf + offset);
            if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03) {
                *out_ep_addr = ep->bEndpointAddress & 0x0F;
                *out_mps = ep->wMaxPacketSize;
                return 1;
            }
        }
        offset += len;
    }
    return 0;
}
 
static int hid_set_idle(uhci_controller_t *hc, usb_device_t *dev, uint8_t iface) {
    return usb_control_transfer(hc, dev, 0x21, HID_REQ_SET_IDLE, 0, iface, 0, 0, 0);
}
 
static int hid_set_protocol_boot(uhci_controller_t *hc, usb_device_t *dev, uint8_t iface) {
    return usb_control_transfer(hc, dev, 0x21, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, iface, 0, 0, 0);
}
 
int hid_mouse_init(hid_mouse_t *m) {
    mouse_zero(m, sizeof(*m));
    usb_mem_init();
 
    pci_device_t pdev;
    if (!pci_find_uhci(&pdev)) {
        t_printf("[mouse] no UHCI controller on PCI bus (VM likely exposes EHCI/xHCI instead)\n");
        return 0;
    }
    t_printf("[mouse] UHCI found: bus=%d slot=%d io_base=%d irq=%d\n",
              pdev.bus, pdev.slot, pdev.bar4 & 0xFFFC, pdev.irq_line);
 
    uint16_t io_base = (uint16_t)(pdev.bar4 & 0xFFFC);
    if (!uhci_init(&m->hc, io_base, pdev.irq_line)) {
        t_printf("[mouse] uhci_init failed\n");
        return 0;
    }
 
    int port = -1;
    if (uhci_port_connected(&m->hc, 0)) port = 0;
    else if (uhci_port_connected(&m->hc, 1)) port = 1;
    if (port < 0) {
        t_printf("[mouse] UHCI ok, but no device connected on port 0 or 1\n");
        return 0;
    }
    t_printf("[mouse] device connected on port %d, resetting...\n", port);
 
    if (!uhci_port_reset(&m->hc, port)) {
        t_printf("[mouse] port reset failed (PORTSC never showed ENABLE)\n");
        return 0;
    }
 
    int is_low_speed = uhci_port_is_lowspeed(&m->hc, port);
    t_printf("[mouse] device speed: low_speed_flag=%d\n", is_low_speed);

    m->dev.address = 0;
    m->dev.max_packet_size0 = 8;
    m->dev.low_speed = is_low_speed ? 1 : 0;
 
    usb_device_descriptor_t devdesc;
    if (usb_get_device_descriptor(&m->hc, &m->dev, &devdesc) != 0) {
        t_printf("[mouse] GET_DESCRIPTOR(Device) failed\n");
        return 0;
    }
    if (devdesc.bMaxPacketSize0 != 0) {
        m->dev.max_packet_size0 = devdesc.bMaxPacketSize0;
    }
    t_printf("[mouse] device desc ok: VID=%d PID=%d class=%d\n",
              devdesc.idVendor, devdesc.idProduct, devdesc.bDeviceClass);
 
    if (usb_set_address(&m->hc, &m->dev, 1) != 0) {
        t_printf("[mouse] SET_ADDRESS failed\n");
        return 0;
    }

    uint8_t cfg_head[9];
    if (usb_control_transfer(&m->hc, &m->dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_CONFIG << 8) | 0, 0, sizeof(cfg_head), cfg_head, 1) != 0) {
        t_printf("[mouse] GET_DESCRIPTOR(Config, head) failed\n");
        return 0;
    }
    uint16_t total_len = cfg_head[2] | (cfg_head[3] << 8);
    if (total_len > 256) total_len = 256;
 
    uint8_t *cfg_full = (uint8_t*)usb_alloc(total_len, 16);
    t_printf("[mouse] cfg_full alloc: ptr=%d (NULL means pool exhausted/bug)\n", (int)(uint32_t)cfg_full);
    if (cfg_full == 0) {
        t_printf("[mouse] usb_alloc returned NULL for cfg_full!\n");
        return 0;
    }
    if (usb_control_transfer(&m->hc, &m->dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_CONFIG << 8) | 0, 0, total_len, cfg_full, 1) != 0) {
        t_printf("[mouse] GET_DESCRIPTOR(Config, full, len=%d) failed\n", total_len);
        return 0;
    }
 
    uint8_t ep_addr = 0, iface_num = 0;
    uint16_t ep_mps = 8;

    t_printf("[mouse] config descriptor raw bytes (total_len=%d):\n", total_len);
    {
        uint16_t dump_len = total_len > 48 ? 48 : total_len;
        for (uint16_t i = 0; i < dump_len; i++) {
            t_printf("%d ", cfg_full[i]);
            if ((i % 8) == 7) t_printf("\n");
        }
        t_printf("\n");
    }
 
    if (!parse_config_for_hid_endpoint(cfg_full, total_len, &ep_addr, &ep_mps, &iface_num)) {
        t_printf("[mouse] no HID interrupt-IN endpoint found in config descriptor\n");
        return 0;
    }
    t_printf("[mouse] HID endpoint found: addr=%d mps=%d iface=%d\n", ep_addr, ep_mps, iface_num);
 
    uint8_t config_value = cfg_full[5];
    if (usb_set_configuration(&m->hc, &m->dev, config_value) != 0) {
        t_printf("[mouse] SET_CONFIGURATION failed\n");
        return 0;
    }
 
    hid_set_idle(&m->hc, &m->dev, iface_num);
    if (hid_set_protocol_boot(&m->hc, &m->dev, iface_num) != 0) {
        t_printf("[mouse] SET_PROTOCOL(boot) failed\n");
        return 0;
    }
 
    m->endpoint_addr = ep_addr;
    m->max_packet = ep_mps > 8 ? 8 : ep_mps;
    m->interface_num = iface_num;
    m->toggle = 0;
 
    t_printf("[mouse] enumeration complete\n");
    return 1;
}
 
int hid_mouse_poll(hid_mouse_t *m, mouse_report_t *out) {
    static uint8_t report_buf[8];
 
    uhci_td_t *td = (uhci_td_t*)usb_alloc(sizeof(uhci_td_t), 16);
    uint32_t maxlen_bits = (m->max_packet == 0) ? 0x7FF : (m->max_packet - 1);
    td->token = ((maxlen_bits & 0x7FF) << 21) |
                ((uint32_t)(m->toggle & 1) << 19) |
                ((uint32_t)(m->endpoint_addr & 0xF) << 15) |
                ((uint32_t)(m->dev.address & 0x7F) << 8) |
                TD_PID_IN;
    td->buffer = usb_virt_to_phys(report_buf);
    td->ctrl_status = TD_STATUS_ACTIVE | (3u << 27) | TD_CTRL_SPD |
                       (m->dev.low_speed ? TD_CTRL_LS : 0);
    td->link = UHCI_PTR_TERMINATE;
 
    uhci_qh_t *qh = (uhci_qh_t*)usb_alloc(sizeof(uhci_qh_t), 16);
    qh->head_link = UHCI_PTR_TERMINATE;
    qh->element_link = usb_virt_to_phys(td);
    qh->sw_pad0 = (uint32_t)td;

    int rc = uhci_submit_qh_and_wait(&m->hc, qh, 20);
    if (rc != 0) {
        return 0; 
    }
 
    m->toggle ^= 1; 

    out->buttons = report_buf[0] & 0x07;
    out->dx = (int8_t)report_buf[1];
    out->dy = (int8_t)report_buf[2];
    out->wheel = (m->max_packet >= 4) ? (int8_t)report_buf[3] : 0;
    return 1;
}