#include "headers/GDT.h"
#include "headers/vga.h"
#include "../standarts_lib/stdio.h"
#include "../system/shell/shell.h"
#include "headers/keyboard.h"
#include "headers/system.h"
#include "headers/framebuffer.h"
#include "headers/IDE_drive.h"
#include <stdint.h>
#include "boot/deb_p.h"
#include "headers/klog.h"
#include "headers/i2c_dw.h"
#include "headers/hardware.h"
#include "headers/head_mouse.h"
#include "headers/mouse.h"
#include "headers/usbfs.h"
#include "../system/shell/graphic/console.h"
#include "../system/GUI/gui.h"
#include "headers/pmm.h"
#include "headers/memory.h"

int blink_counter = 0;
extern int shell_row, shell_col;

static void boot_hold_ms(uint32_t ms)
{
    uint64_t end = i2c_now() + (uint64_t)ms * i2c_cycles_per_ms();
    while (i2c_now() < end)
        __asm__ volatile ("pause");
}

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

#define AHCI_BASE_ADDRESS 0xFEBF1000

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
       while (1);

    if (!pmm_init(mbi_addr)) {
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    kheap_init();

    framebuffer_init(mbi_addr);

    if (framebuffer_addr == 0)
        while (1);

    console_init_geometry();

    klog_init();                   /* TSC-годинник журналу: час 0 — тут */
    clear_screen(0x000000);
    console_reset();
    shell_row = TOP_ROWS;          /* завантажувальний журнал іде в консоль під верхньою панеллю */
    shell_col = 0;
    klog_screen(1);

    boot_log_early(magic);
    main_g();                      /* GDT.c: будує дескриптори (більше нічого не друкує) */

    hardware_init(mbi_addr);
    ide_init(0, 0, 0, 0, 0);
    boot_log_hw();

    mouse_init();                  /* заодно скануємо xHCI: миші та флешки (скан один, див. mouse.c) */
    usbfs_autostart();             /* змонтувати флешки, якщо є — usb scan вручну не потрібен */
    boot_log_usb();
    boot_log_done();

    boot_hold_ms(1200);            /* дати встигнути прочитати журнал; усе це є в `dmesg` */
    klog_screen(0);
    cmd_gui();

    static unsigned char drive_buf[512];
    ide_read_sectors(0, 1, 0, drive_buf);

    while (1)
        {
            keyboard_handler();
            blink_cursor();
            if (mouse_poll()) {                    /* тачпад/миша гортають історію текстового шела */
                const mouse_state_t *m = mouse_get();
                if (m->wheel) console_scroll_view(m->wheel * 3);
            }
        }
    if (package[0] == 0)
    {
        t_printf("MBR signature: %x %x\n", drive_buf[510], drive_buf[511]);
    }
    else
    {
        t_printf("ide_read_sectors failed, code %d\n", package[0]);
    }
   return;
}