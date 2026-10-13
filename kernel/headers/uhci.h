#ifndef UHCI_H
#define UHCI_H

#include <stdint.h>
#include "PCI.h"

/* --- регістри (зсув від io_base) --- */
#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FRBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

#define UHCI_CMD_RS      0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004
#define UHCI_CMD_CF      0x0040
#define UHCI_CMD_MAXP    0x0080

#define UHCI_STS_HCHALTED 0x0020
#define UHCI_STS_ALL      0x003F

#define UHCI_PORTSC_CONNECT       0x0001
#define UHCI_PORTSC_CONNECT_CHG   0x0002
#define UHCI_PORTSC_ENABLE        0x0004
#define UHCI_PORTSC_ENABLE_CHG    0x0008
#define UHCI_PORTSC_LOWSPEED      0x0100
#define UHCI_PORTSC_RESET         0x0200
#define UHCI_PORTSC_OC_CHG        0x0800
/* біти "write 1 to clear": їх не можна випадково писати назад */
#define UHCI_PORTSC_RWC (UHCI_PORTSC_CONNECT_CHG | UHCI_PORTSC_ENABLE_CHG | UHCI_PORTSC_OC_CHG)

/* PCI config: Legacy Support (BIOS емулює PS/2 через SMI — треба вимкнути) */
#define UHCI_PCI_LEGSUP 0xC0

typedef struct uhci_td {
    volatile uint32_t link;
    volatile uint32_t ctrl_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t sw_next;
    uint32_t sw_buffer_virt;
    uint32_t sw_reserved0;
    uint32_t sw_reserved1;
} __attribute__((packed, aligned(16))) uhci_td_t;

typedef struct uhci_qh {
    volatile uint32_t head_link;
    volatile uint32_t element_link;
    uint32_t sw_pad0;
    uint32_t sw_pad1;
} __attribute__((packed, aligned(16))) uhci_qh_t;

#define UHCI_PTR_TERMINATE 0x1
#define UHCI_PTR_QH        0x2
#define UHCI_PTR_DEPTH     0x4

#define TD_CTRL_ACTLEN_MASK  0x7FF
#define TD_CTRL_STATUS_SHIFT 16
#define TD_STATUS_ACTIVE     (1<<7)
#define TD_STATUS_STALLED    (1<<6)
#define TD_STATUS_DBUFERR    (1<<5)
#define TD_STATUS_BABBLE     (1<<4)
#define TD_STATUS_NAK        (1<<3)
#define TD_STATUS_CRCTIMEOUT (1<<2)
#define TD_STATUS_BITSTUFF   (1<<1)
#define TD_STATUS_ERRORS     (TD_STATUS_STALLED | TD_STATUS_DBUFERR | TD_STATUS_BABBLE | \
                              TD_STATUS_CRCTIMEOUT | TD_STATUS_BITSTUFF)
#define TD_CTRL_C_ERR3       (3u<<27)
#define TD_CTRL_IOC          (1u<<24)
#define TD_CTRL_LS           (1u<<26)
#define TD_CTRL_SPD          (1u<<29)

#define TD_PID_SETUP 0x2D
#define TD_PID_IN    0x69
#define TD_PID_OUT   0xE1

/* коди повернення транзакцій */
#define UHCI_OK        0
#define UHCI_ERR_XFER (-1)   /* CRC / timeout / babble / bitstuff */
#define UHCI_ERR_TIME (-2)   /* пристрій не відповів за відведений час */
#define UHCI_ERR_STALL (-3)  /* пристрій відповів STALL (запит не підтримується) */
#define UHCI_ERR_ARGS (-4)

typedef struct {
    uint16_t io_base;
    uint8_t  irq_line;
    uint8_t  num_ports;
    uint8_t  pci_bus, pci_slot, pci_func;
} uhci_controller_t;

/* точна затримка (PIT канал 2), працює без переривань */
void uhci_delay_ms(uint32_t ms);

int  uhci_init(uhci_controller_t *hc, const pci_device_t *pdev);
void uhci_stop(uhci_controller_t *hc);

int  uhci_port_connected(uhci_controller_t *hc, int port);
int  uhci_port_is_lowspeed(uhci_controller_t *hc, int port);
int  uhci_port_reset(uhci_controller_t *hc, int port);
void uhci_port_disable(uhci_controller_t *hc, int port);

/* Control-транзакція (SETUP + DATA... + STATUS). data — будь-який буфер,
 * ми самі копіюємо через DMA-буфер. */
int uhci_control_transfer(uhci_controller_t *hc, uint8_t addr, int low_speed, uint16_t mps0,
                          const uint8_t setup[8], void *data, uint16_t len, int dir_in);

/* Постійна interrupt-IN черга (для мишки). Ніщо не блокує. */
int uhci_int_start(uhci_controller_t *hc, uint8_t addr, int low_speed,
                   uint8_t ep, uint16_t maxlen, uint8_t interval);
/* 1 — прийшов пакет (скопійований у out, довжина в *len);
 * 0 — даних поки немає; <0 — помилка транзакції (чергу вже перезаряджено) */
int uhci_int_poll(uhci_controller_t *hc, uint8_t *out, uint8_t *len);
void uhci_int_stop(uhci_controller_t *hc);

#endif
