#include "../../../headers/usb.h"
#include "../../../headers/usb_mem.h"

#define MAX_CTRL_TDS 16  /* вистачає на SETUP + (wLength/8 округлено) + STATUS */
 
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_packet_t;
 
static uint32_t make_token(uint16_t max_len_field, uint8_t endpoint, uint8_t addr, uint8_t pid, uint8_t toggle) {
    uint32_t maxlen_bits = (max_len_field == 0) ? 0x7FF : (uint32_t)(max_len_field - 1);
    return ((maxlen_bits & 0x7FF) << 21) |
           ((uint32_t)(toggle & 1) << 19) |
           ((uint32_t)(endpoint & 0xF) << 15) |
           ((uint32_t)(addr & 0x7F) << 8) |
           (uint32_t)pid;
}
 
int usb_control_transfer(uhci_controller_t *hc, usb_device_t *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data, int dev_to_host) {
 
    usb_setup_packet_t *setup = (usb_setup_packet_t*)usb_alloc(sizeof(usb_setup_packet_t), 16);
    t_printf("[usb] setup ptr=%d req=%d wLen=%d data_ptr=%d\n", (int)(uint32_t)setup, bRequest, wLength, (int)(uint32_t)data);
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;
 
    uhci_td_t *tds[MAX_CTRL_TDS];
    int n = 0;
 
    /* --- SETUP TD (завжди DATA0) --- */
    uhci_td_t *td_setup = (uhci_td_t*)usb_alloc(sizeof(uhci_td_t), 16);
    td_setup->token = make_token(sizeof(usb_setup_packet_t), 0, dev->address, TD_PID_SETUP, 0);
    td_setup->buffer = usb_virt_to_phys(setup);
    td_setup->ctrl_status = TD_STATUS_ACTIVE | (3u << 27) | (dev->low_speed ? TD_CTRL_LS : 0);
    tds[n++] = td_setup;
 
    /* --- DATA стадія (0 або більше TD по max_packet_size0 байт) --- */
    uint8_t toggle = 1;
    uint16_t remaining = wLength;
    uint8_t *cursor = (uint8_t*)data;
    uint8_t data_pid = dev_to_host ? TD_PID_IN : TD_PID_OUT;
 
    while (remaining > 0 && n < MAX_CTRL_TDS - 1) {
        uint16_t chunk = remaining > dev->max_packet_size0 ? dev->max_packet_size0 : remaining;
        uhci_td_t *td = (uhci_td_t*)usb_alloc(sizeof(uhci_td_t), 16);
        td->token = make_token(chunk, 0, dev->address, data_pid, toggle);
        td->buffer = usb_virt_to_phys(cursor);
        /* БЕЗ SPD: у control-transfer довжина кожного чанка вже точно
         * порахована з wLength, SPD тут лише шкодить (передчасно рве
         * ланцюг на STATUS-стадії при цілком нормальному короткому
         * останньому пакеті). */
        td->ctrl_status = TD_STATUS_ACTIVE | (3u << 27) | (dev->low_speed ? TD_CTRL_LS : 0);
        tds[n++] = td;
 
        cursor += chunk;
        remaining -= chunk;
        toggle ^= 1;
    }
 
    /* --- STATUS TD (зворотній напрямок, завжди DATA1, нульова довжина) --- */
    uint8_t status_pid = (wLength == 0 || !dev_to_host) ? TD_PID_IN : TD_PID_OUT;
    uhci_td_t *td_status = (uhci_td_t*)usb_alloc(sizeof(uhci_td_t), 16);
    td_status->token = make_token(0, 0, dev->address, status_pid, 1);
    td_status->buffer = 0;
    td_status->ctrl_status = TD_STATUS_ACTIVE | (3u << 27) | TD_CTRL_IOC | (dev->low_speed ? TD_CTRL_LS : 0);
    tds[n++] = td_status;
 
    /* --- Зв'язати TD в ланцюг (depth-first, щоб виконувались підряд в тому ж кадрі) --- */
    for (int i = 0; i < n - 1; i++) {
        tds[i]->link = usb_virt_to_phys(tds[i+1]) | UHCI_PTR_DEPTH;
    }
    tds[n-1]->link = UHCI_PTR_TERMINATE;
 
    /* --- QH, що вказує на перший TD --- */
    uhci_qh_t *qh = (uhci_qh_t*)usb_alloc(sizeof(uhci_qh_t), 16);
    qh->head_link = UHCI_PTR_TERMINATE;
    qh->element_link = usb_virt_to_phys(tds[0]);
    qh->sw_pad0 = (uint32_t)tds[n-1]; /* останній TD містить IOC — по ньому й чекаємо завершення */
 
    /* Спрощення: чекаємо завершення КОЖНОГО TD послідовно, перевіряючи їх по
     * черзі, бо uhci_submit_qh_and_wait стежить лише за одним TD (sw_pad0).
     * Для короткого control-transfer (пара TD) це працює надійно, бо HC
     * виконує весь depth-first ланцюг сам, поки контролер в Run-стані. */
    int rc = uhci_submit_qh_and_wait(hc, qh, 500);
 
    /* Детальна діагностика КОЖНОГО TD — незалежно від rc, щоб бачити
     * реальний стан, а не тільки підсумкове "успіх/невдача". */
    t_printf("[usb] ctrl transfer req=%d n_td=%d rc=%d\n", bRequest, n, rc);
    for (int i = 0; i < n; i++) {
        uint32_t cs = tds[i]->ctrl_status;
        int active = (cs & TD_STATUS_ACTIVE) ? 1 : 0;
        int err = (cs >> TD_CTRL_STATUS_SHIFT) & 0x1F; /* біти 17-21: bitstuff/crc/nak/babble/dbuf */
        int actual_len = cs & TD_CTRL_ACTLEN_MASK;
        t_printf("[usb]  td[%d] active=%d err_bits=%d actual_len=%d\n", i, active, err, actual_len);
    }
 
    if (rc != 0) return rc;
 
    /* Перевірка, що ВСІ TD дійсно завершились без помилок І дійсно
     * виконались (Active мусить бути знятий, а не просто "без помилки" —
     * TD, який ніколи не запускався, теж має Active=1). */
    for (int i = 0; i < n; i++) {
        uint32_t status = (tds[i]->ctrl_status >> TD_CTRL_STATUS_SHIFT) & 0xFF;
        if (tds[i]->ctrl_status & TD_STATUS_ACTIVE) {
            return -3; /* TD ніколи не виконався */
        }
        if (status & (TD_STATUS_STALLED | TD_STATUS_DBUFERR | TD_STATUS_BABBLE | TD_STATUS_CRCTIMEOUT | TD_STATUS_BITSTUFF)) {
            return -1;
        }
    }
    return 0;
}
 
int usb_get_device_descriptor(uhci_controller_t *hc, usb_device_t *dev, usb_device_descriptor_t *out) {
    return usb_control_transfer(hc, dev,
        0x80 /* IN, standard, device */, USB_REQ_GET_DESCRIPTOR,
        (USB_DESC_DEVICE << 8) | 0, 0, sizeof(usb_device_descriptor_t),
        out, 1);
}
 
int usb_set_address(uhci_controller_t *hc, usb_device_t *dev, uint8_t new_addr) {
    int rc = usb_control_transfer(hc, dev,
        0x00 /* OUT, standard, device */, USB_REQ_SET_ADDRESS,
        new_addr, 0, 0, 0, 0);
    if (rc == 0) {
        dev->address = new_addr; /* USB spec: пристрій приймає нову адресу після STATUS stage */
    }
    return rc;
}
 
int usb_set_configuration(uhci_controller_t *hc, usb_device_t *dev, uint8_t config_value) {
    return usb_control_transfer(hc, dev,
        0x00, USB_REQ_SET_CONFIGURATION,
        config_value, 0, 0, 0, 0);
}
 