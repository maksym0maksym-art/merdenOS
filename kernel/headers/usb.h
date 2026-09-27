#ifndef USB_H
#define USB_H

#include <stdint.h>
#include "uhci.h"

typedef struct {
    uint8_t address;
    uint8_t max_packet_size0;
    uint8_t low_speed;
} usb_device_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

#define USB_REQ_GET_DESCRIPTOR   0x06
#define USB_REQ_SET_ADDRESS      0x05
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_DESC_DEVICE  0x01
#define USB_DESC_CONFIG  0x02

#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_REQ_SET_IDLE     0x0A
#define HID_PROTOCOL_BOOT    0

int usb_control_transfer(uhci_controller_t *hc, usb_device_t *dev,
                          uint8_t bmRequestType, uint8_t bRequest,
                          uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                          void *data, int dev_to_host);

int usb_get_device_descriptor(uhci_controller_t *hc, usb_device_t *dev, usb_device_descriptor_t *out);
int usb_set_address(uhci_controller_t *hc, usb_device_t *dev, uint8_t new_addr);
int usb_set_configuration(uhci_controller_t *hc, usb_device_t *dev, uint8_t config_value);

#endif