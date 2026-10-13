#include "../../../headers/usb.h"
#include <stdint.h>

/* Формуємо 8-байтний SETUP-пакет і віддаємо контролеру.
 * Вся робота з TD/QH тепер у UHCI.c (uhci_control_transfer). */
int usb_control_transfer(uhci_controller_t *hc, usb_device_t *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data, int dev_to_host) {
    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(wLength & 0xFF);
    setup[7] = (uint8_t)(wLength >> 8);

    return uhci_control_transfer(hc, dev->address, dev->low_speed, dev->max_packet_size0,
                                 setup, data, wLength, dev_to_host);
}

int usb_get_descriptor(uhci_controller_t *hc, usb_device_t *dev,
                       uint8_t type, uint8_t index, uint16_t len, void *out) {
    return usb_control_transfer(hc, dev,
        0x80 /* IN, standard, device */, USB_REQ_GET_DESCRIPTOR,
        (uint16_t)((type << 8) | index), 0, len, out, 1);
}

int usb_get_device_descriptor(uhci_controller_t *hc, usb_device_t *dev, usb_device_descriptor_t *out) {
    return usb_get_descriptor(hc, dev, USB_DESC_DEVICE, 0, sizeof(usb_device_descriptor_t), out);
}

int usb_set_address(uhci_controller_t *hc, usb_device_t *dev, uint8_t new_addr) {
    int rc = usb_control_transfer(hc, dev,
        0x00 /* OUT, standard, device */, USB_REQ_SET_ADDRESS,
        new_addr, 0, 0, 0, 0);
    if (rc == 0) {
        dev->address = new_addr;
        uhci_delay_ms(10);          /* пристрій має право "перемикатись" до 2 мс */
    }
    return rc;
}

int usb_set_configuration(uhci_controller_t *hc, usb_device_t *dev, uint8_t config_value) {
    return usb_control_transfer(hc, dev,
        0x00, USB_REQ_SET_CONFIGURATION,
        config_value, 0, 0, 0, 0);
}
