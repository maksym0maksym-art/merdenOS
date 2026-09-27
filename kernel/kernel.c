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
#include "headers/hardware.h"
#include "headers/head_mouse.h"

int blink_counter = 0;

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

#define AHCI_BASE_ADDRESS 0xFEBF1000

void kernel_main(uint32_t magic, uint32_t mbi_addr)
{
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
       while (1);

    framebuffer_init(mbi_addr);

    if (framebuffer_addr == 0)
        while (1);

    main_g();

    debug_power();
    
    clear_screen(0x000000);
    shell_init();

    hardware_init(mbi_addr);
    ide_init(0, 0, 0, 0, 0);

    static unsigned char drive_buf[512];
    ide_read_sectors(0, 1, 0, drive_buf);

    while (1)
        {
            keyboard_handler();
            blink_cursor();
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