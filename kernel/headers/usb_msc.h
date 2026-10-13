#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdint.h>
#include "xhci.h"

#define MSC_MAX 4

#define MSC_OK       0
#define MSC_E_IO    (-1)       /* помилка транспорту (відключено, таймаут, phase error) */
#define MSC_E_SCSI  (-2)       /* пристрій відповів CHECK CONDITION; sense_key/asc/ascq заповнені */
#define MSC_E_ARGS  (-3)
#define MSC_E_NOMED (-4)       /* носія нема (порожній карт-рідер) */
#define MSC_E_WP    (-5)       /* захист від запису */

typedef struct usb_msc {
    int          used;
    xhci_dev_t  *dev;
    uint8_t      iface;
    uint32_t     tag;
    uint32_t     block_size;
    uint32_t     block_count;
    char         vendor[9], product[17], rev[5];
    uint8_t      sense_key, asc, ascq;
    int          write_protected;
} usb_msc_t;

/* Колбек для xhci_scan(): впізнає Mass Storage, налаштовує endpoint'и, ініціалізує SCSI. */
int  usb_msc_probe(xhci_dev_t *dev, void *arg);

int  usb_msc_count(void);
usb_msc_t *usb_msc_get(int i);
void usb_msc_forget_all(void);

/* ctx = usb_msc_t* — підходить як blockdev_t.read/write/flush */
int  usb_msc_read(void *ctx, uint32_t lba, uint32_t count, void *buf);
int  usb_msc_write(void *ctx, uint32_t lba, uint32_t count, const void *buf);
int  usb_msc_sync(void *ctx);

/* START STOP UNIT (eject), якщо пристрій знімний */
int  usb_msc_eject(usb_msc_t *m);

const char *usb_msc_err_str(int err);

#endif
