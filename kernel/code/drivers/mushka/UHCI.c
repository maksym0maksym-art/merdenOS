#include "../../../headers/uhci.h"
#include "../../../headers/usb_mem.h"
#include "../../../headers/ports.h"
 
static void delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms * 20000u; i++) {
        __asm__ volatile ("nop");
    }
}
 
static uint32_t *frame_list = 0;
 
int uhci_init(uhci_controller_t *hc, uint16_t io_base, uint8_t irq_line) {
    hc->io_base = io_base;
    hc->irq_line = irq_line;
 
    outw(io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
    delay_ms(50);
    outw(io_base + UHCI_USBCMD, 0);
 
    outw(io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
    delay_ms(10);
    for (int i = 0; i < 1000; i++) {
        if ((inw(io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0) break;
    }

    frame_list = (uint32_t*)usb_alloc(4096, 4096);
    for (int i = 0; i < 1024; i++) {
        frame_list[i] = UHCI_PTR_TERMINATE;
    }
 
    outl(io_base + UHCI_FRBASEADD, usb_virt_to_phys(frame_list));
    outw(io_base + UHCI_FRNUM, 0);
    outw(io_base + UHCI_SOFMOD, 64);
 
    outw(io_base + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP);
 
    return 1;
}
 
int uhci_port_connected(uhci_controller_t *hc, int port) {
    uint16_t reg = hc->io_base + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2);
    return (inw(reg) & UHCI_PORTSC_CONNECT) != 0;
}
 
int uhci_port_is_lowspeed(uhci_controller_t *hc, int port) {
    uint16_t reg = hc->io_base + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2);
    return (inw(reg) & UHCI_PORTSC_LOWSPEED) != 0;
}
 
int uhci_port_reset(uhci_controller_t *hc, int port) {
    uint16_t reg = hc->io_base + (port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2);
 
    uint16_t val = inw(reg);
    outw(reg, val | UHCI_PORTSC_RESET);
    delay_ms(50); 
    outw(reg, inw(reg) & ~UHCI_PORTSC_RESET);
    delay_ms(10);
 
    val = inw(reg);
    outw(reg, val | UHCI_PORTSC_ENABLE);
    delay_ms(10);
 
    for (int i = 0; i < 100; i++) {
        val = inw(reg);
        if (val & UHCI_PORTSC_ENABLE) return 1;
        outw(reg, val | UHCI_PORTSC_ENABLE);
        delay_ms(5);
    }
    return 0;
}
 
int uhci_submit_qh_and_wait(uhci_controller_t *hc, uhci_qh_t *qh, uint32_t timeout_ms) {
    (void)hc;
    frame_list[0] = usb_virt_to_phys(qh) | UHCI_PTR_QH;
 
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        uhci_td_t *td = (uhci_td_t*)(qh->sw_pad0); /* перший TD зберігаємо тут */
        if (td == 0) break;
        uint32_t status = (td->ctrl_status >> TD_CTRL_STATUS_SHIFT) & 0xFF;
        if (!(status & TD_STATUS_ACTIVE)) {
            frame_list[0] = UHCI_PTR_TERMINATE;
            if (status & (TD_STATUS_STALLED | TD_STATUS_DBUFERR | TD_STATUS_BABBLE | TD_STATUS_CRCTIMEOUT | TD_STATUS_BITSTUFF)) {
                return -1; 
            }
            return 0; 
        }
        delay_ms(1);
        waited++;
    }
    frame_list[0] = UHCI_PTR_TERMINATE;
    return -2;
}