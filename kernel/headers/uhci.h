#ifndef UHCI_H
#define UHCI_H
 
#include <stdint.h>

#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FRBASEADD  0x08
#define UHCI_SOFMOD     0x0C
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12
 
#define UHCI_CMD_RS     0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004
#define UHCI_CMD_MAXP    0x0080
 
#define UHCI_PORTSC_CONNECT       0x0001
#define UHCI_PORTSC_CONNECT_CHG   0x0002
#define UHCI_PORTSC_ENABLE        0x0004
#define UHCI_PORTSC_ENABLE_CHG    0x0008
#define UHCI_PORTSC_LOWSPEED      0x0100
#define UHCI_PORTSC_RESET         0x0200
 
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
 
#define TD_CTRL_ACTLEN_MASK 0x7FF
#define TD_CTRL_STATUS_SHIFT 16
#define TD_STATUS_ACTIVE    (1<<7)
#define TD_STATUS_STALLED   (1<<6)
#define TD_STATUS_DBUFERR   (1<<5)
#define TD_STATUS_BABBLE    (1<<4)
#define TD_STATUS_NAK       (1<<3)
#define TD_STATUS_CRCTIMEOUT (1<<2)
#define TD_STATUS_BITSTUFF  (1<<1)
#define TD_CTRL_IOC         (1u<<24)
#define TD_CTRL_LS          (1u<<26) 
#define TD_CTRL_SPD         (1u<<29)  
 
#define TD_PID_SETUP 0x2D
#define TD_PID_IN    0x69
#define TD_PID_OUT   0xE1
 
typedef struct {
    uint16_t io_base;
    uint8_t  irq_line;
} uhci_controller_t;
 
int  uhci_init(uhci_controller_t *hc, uint16_t io_base, uint8_t irq_line);
int  uhci_port_reset(uhci_controller_t *hc, int port);
int  uhci_port_connected(uhci_controller_t *hc, int port);

int  uhci_port_is_lowspeed(uhci_controller_t *hc, int port);

int uhci_submit_qh_and_wait(uhci_controller_t *hc, uhci_qh_t *qh, uint32_t timeout_ms);
 
#endif
 