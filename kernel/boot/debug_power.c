#include <stdint.h>
#include "../headers/klog.h"
#include "../headers/framebuffer.h"
#include "../headers/hardware.h"
#include "../headers/IDE_drive.h"
#include "../headers/rtc.h"
#include "../headers/usb_msc.h"
#include "../headers/usbfs.h"
#include "../../system/shell/graphic/console.h"
#include "deb_p.h"

void boot_log_early(uint32_t magic)
{
    klog("Merden Rose Kernel, multiboot2? framebuffer");
    klog_ok("multiboot2: bootloader magic 0x%X verified", magic);
    klog_ok("framebuffer: %ux%u, %u bpp, pitch %u", framebuffer_width, framebuffer_height,
            (uint32_t)framebuffer_bpp, framebuffer_pitch);
    klog("console: text grid %dx%d (glyph %dx%d px)", console_cols, console_rows, CHAR_WIDTH, CHAR_HEIGHT);
    klog_ok("GDT: 5 segment descriptors built (null, kernel code/data, user code/data)");
}

void boot_log_hw(void)
{
    klog("cpu: %s, %s", hardware_get_cpu_vendor(), hardware_get_cpu_brand());
    int cpus = hardware_get_logical_cpus();
    if (cpus > 0) klog("cpu: %d logical processor(s)", cpus);
    else          klog_warn("cpu: logical CPU count unknown");

    uint32_t mb = (uint32_t)(hardware_get_ram_bytes() >> 20);
    if (mb) klog_ok("memory: %u MB usable (multiboot2 memory map)", mb);
    else    klog_warn("memory: size not reported by bootloader");

    const char *name = hardware_get_computer_name();
    if (name && name[0]) klog("machine: %s", name);

    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (!ide_devices[i].Reserved) continue;
        n++;
        klog("ide%d: %s \"%s\", %u MB", i, ide_devices[i].Type == 0x01 /* IDE_ATAPI */ ? "ATAPI" : "ATA",
             ide_devices[i].Model, (uint32_t)(ide_devices[i].Size >> 11));
    }
    if (n) klog_ok("ide: %d device(s) found", n);
    else   klog_warn("ide: no legacy ATA/ATAPI devices (NVMe/AHCI disks are not supported yet)");
}


void boot_log_usb(void)
{
    int n = usb_msc_count();
    if (n == 0) { klog("usb: no mass-storage devices attached"); return; }
    if (usbfs_mounted()) klog_ok("usb: %d flash drive(s) found, filesystem mounted", n);
    else                 klog_warn("usb: %d flash drive(s) found, no FAT32/exFAT filesystem", n);
}

void boot_log_done(void)
{
    RTC_TIME t = rtc_get_time();
    int y = t.year < 100 ? 2000 + t.year : t.year;
    klog("rtc: %04d-%02d-%02d %02d:%02d:%02d (CMOS clock)", y, t.month, t.day, t.hour, t.minute, t.second);
    klog_ok("boot complete in %u ms - starting desktop (type `dmesg` to see this log again)", klog_uptime_ms());
}