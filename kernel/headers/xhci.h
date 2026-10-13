#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/*
 * xHCI (USB 3.x / 2.0 root hub на сучасних ноутбуках). Працює полінгом, без переривань,
 * DMA — статичні буфери в .bss (ядро ідентично відображене, phys == virt).
 *
 * Підтримується: root-порти (без зовнішніх хабів), speed LS/FS/HS/SS, control + bulk.
 */

#define XHCI_MAX_DEVS   6        /* скільки пристроїв одночасно тримаємо відкритими */

#define XHCI_OK          0
#define XHCI_E_TIMEOUT  (-1)     /* пристрій/контролер не відповів */
#define XHCI_E_STALL    (-2)     /* STALL (endpoint у стані Halted) */
#define XHCI_E_XFER     (-3)     /* помилка транзакції/babble/інше */
#define XHCI_E_NODEV    (-4)     /* пристрій відключено / контролера нема */
#define XHCI_E_ARGS     (-5)
#define XHCI_E_NOMEM    (-6)
#define XHCI_E_CMD      (-7)     /* контролер відхилив команду */

typedef struct {
    volatile uint32_t *trb;      /* 4 dword на TRB */
    uint32_t phys;
    uint32_t enq;                /* індекс наступного вільного TRB */
    uint32_t cycle;              /* producer cycle state */
    uint32_t size;               /* кількість TRB, включно з Link */
} xhci_ring_t;

typedef struct xhci_dev {
    int      used;
    int      idx;                /* індекс у внутрішній таблиці */
    int      slot;               /* slot id (1..) */
    int      port;               /* root-порт (1..) */
    int      speed;              /* PORTSC speed: 1=FS 2=LS 3=HS 4=SS 5=SS+ */
    uint16_t mps0;
    uint16_t vid, pid;
    uint8_t  dclass, dsub, dproto;
    uint8_t  nconf;
    uint8_t  ep_in_addr, ep_out_addr;   /* адреси bulk-ендпоінтів (0x81, 0x02 ...) після xhci_configure_bulk */
    int      dci_in, dci_out;
    uint16_t mps_in, mps_out;
    xhci_ring_t ep0, ring_in, ring_out;
    uint32_t last_cc;            /* останній completion code (для діагностики) */
    /* interrupt IN endpoint (HID): одна передача "в польоті" */
    uint8_t  intr_armed, intr_done;
    uint32_t intr_trb, intr_cc, intr_resid, intr_len;
} xhci_dev_t;

/* Усі пристрої, що бачив останній xhci_scan (також ті, що були відпущені: клавіатури, хаби, камери...). */
#define XHCI_SEEN_MAX 16
typedef struct {
    int      port, speed;                 /* speed: 1=FS 2=LS 3=HS 4=SS 5=SS+ */
    uint16_t vid, pid;
    uint8_t  dclass, dsub, dproto;
    uint8_t  claimed;                     /* 1 — драйвер (Mass Storage) забрав пристрій */
    uint8_t  failed;                      /* 1 — пристрій підключено, але прочитати не вдалося */
    char     manufacturer[32], product[48];
} xhci_seen_t;

int  xhci_seen_count(void);
const xhci_seen_t *xhci_seen_get(int i);
const char *xhci_speed_str(int speed);    /* "USB 3.0 5Gbps" тощо */

/* Колбек: викликається для кожного знайденого пристрою (вже має адресу, device descriptor прочитано).
 * Повертає 1 — залишити пристрій відкритим, 0 — відпустити (Disable Slot). */
typedef int (*xhci_probe_fn)(xhci_dev_t *dev, void *arg);

/* Знаходить xHCI-контролер(и), скидає, піднімає, перебирає порти.
 * Повертає: кількість залишених пристроїв (>=0) або від'ємний код, якщо контролера нема / він не піднявся. */
int  xhci_scan(xhci_probe_fn probe, void *arg);

/* Control-передача на EP0. data — буфер (будь-яка адреса в перших 4 ГБ).
 * Повертає кількість переданих байт (>=0) або XHCI_E_*. */
int  xhci_control(xhci_dev_t *d, uint8_t bmRequestType, uint8_t bRequest,
                  uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength);

/* Налаштувати пару bulk-endpoint'ів (Configure Endpoint) + SET_CONFIGURATION(config_value). */
int  xhci_configure_bulk(xhci_dev_t *d, uint8_t config_value,
                         uint8_t in_addr,  uint16_t in_mps,  uint8_t in_burst,
                         uint8_t out_addr, uint16_t out_mps, uint8_t out_burst);

/* Bulk-передача (len <= 64 КБ). *actual — скільки реально передано. Повертає 0 або XHCI_E_*. */
int  xhci_bulk(xhci_dev_t *d, int is_in, void *buf, uint32_t len, uint32_t *actual, uint32_t timeout_ms);

/* Зняти STALL з endpoint'а: Reset Endpoint + CLEAR_FEATURE(HALT) + скидання кільця. */
int  xhci_clear_halt(xhci_dev_t *d, int is_in);

/* Відновити endpoint після таймауту/помилки (Stop/Reset + Set TR Dequeue). */
int  xhci_recover_ep(xhci_dev_t *d, int is_in);

/* ---- Interrupt IN (миші/клавіатури HID). Polling, одна передача в польоті. ---- */
/* Налаштувати interrupt IN endpoint (ring_in/dci_in) + SET_CONFIGURATION. binterval — з дескриптора endpoint. */
int  xhci_configure_intr_in(xhci_dev_t *d, uint8_t config_value, uint8_t ep_addr, uint16_t mps, uint8_t binterval);
/* Поставити в чергу прийом len байт у buf (buf — у перших 4 ГБ, не перетинає межу 64 КБ). */
int  xhci_intr_submit(xhci_dev_t *d, void *buf, uint32_t len);
/* Не блокує. 1 — дані прийшли (*actual байт), 0 — ще нічого, <0 — помилка (виклич xhci_intr_recover і submit знову). */
int  xhci_intr_poll(xhci_dev_t *d, uint32_t *actual);
int  xhci_intr_recover(xhci_dev_t *d);

/* Зупинити контролер (RS=0). Викликати, якщо USB більше не потрібен. */
void xhci_shutdown(void);

/* Коротка інформація про контролер (для `usb info`). */
void xhci_print_info(void);

#endif
