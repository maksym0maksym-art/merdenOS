#ifndef EMU_XHCI_H
#define EMU_XHCI_H
#include <stdint.h>

#define EMU_DEV_MSC 1
#define EMU_DEV_KBD 2
#define EMU_DEV_MOUSE 3

typedef struct {
    int csz;                       /* 32 або 64 */
    int bios_running;
    int verbose;
    /* лічильники для перевірок */
    long mmio_reads, mmio_writes, writes_while_off, doorbells, doorbell_no_bme, doorbell_halted, bad_doorbells, halted_doorbells;
    long events_posted, cmds, param_errors, bad_cmds, bad_trbs, bad_cbw, bad_run, crossed_64k;
    long hc_resets, reset_while_running, handoffs, port_resets, warm_resets;
    long control_xfers, bulk_tds, cbws, csws, set_configs, bot_resets, clear_halts;
    long set_protocol, set_idle, intr_reports;
    long stalls, babbles, dropped, naks, ep_resets, ep_stops, set_deqs, mps0_updates, slots_disabled;
} emu_cfg_t;

extern emu_cfg_t emu;

void emu_reset(void);
void emu_power_on(int csz, int bios_running);
int  emu_add_msc(int port, int speed, const char *image, uint32_t bsz);
void emu_add_kbd(int port, int speed, int mps0);
void emu_add_mouse(int port, int speed, int mps0);
void emu_mouse_move(int port, int dx, int dy, int buttons, int wheel);
void emu_msc_set(int port, int no_medium, int wp, int tur_not_ready);
void emu_msc_inject(int port, int stall_in, int stall_out, int drop_in);
void emu_unplug(int port);
uint64_t emu_sectors_written(int port);
int  emu_hc_ready_for_run(void);

#endif
