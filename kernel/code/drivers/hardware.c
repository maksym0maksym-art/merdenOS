#include "../../headers/hardware.h"

#include "../../../system/shell/shell.h"

/* =======
   CPU
========*/

static char cpu_vendor[13];
static char cpu_brand[49];

static uint32_t cpu_max_basic_leaf;
static uint32_t cpu_max_extended_leaf;

static int cpu_logical_cpus = 1;


static void cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
)
{
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax),
          "=b"(*ebx),
          "=c"(*ecx),
          "=d"(*edx)
        : "a"(leaf),
          "c"(subleaf)
    );
}


static void detect_cpu(void)
{
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    cpuid(
        0,
        0,
        &eax,
        &ebx,
        &ecx,
        &edx
    );

    cpu_max_basic_leaf = eax;


    ((uint32_t *)cpu_vendor)[0] = ebx;
    ((uint32_t *)cpu_vendor)[1] = edx;
    ((uint32_t *)cpu_vendor)[2] = ecx;

    cpu_vendor[12] = '\0';

    cpuid(
        0x80000000,
        0,
        &eax,
        &ebx,
        &ecx,
        &edx
    );

    cpu_max_extended_leaf = eax;

    if (cpu_max_extended_leaf >= 0x80000004)
    {
        uint32_t *brand =
            (uint32_t *)cpu_brand;

        cpuid(
            0x80000002,
            0,
            &brand[0],
            &brand[1],
            &brand[2],
            &brand[3]
        );

        cpuid(
            0x80000003,
            0,
            &brand[4],
            &brand[5],
            &brand[6],
            &brand[7]
        );

        cpuid(
            0x80000004,
            0,
            &brand[8],
            &brand[9],
            &brand[10],
            &brand[11]
        );

        cpu_brand[48] = '\0';

        // Intel часто лишає провідні пробіли у brand string — прибираємо їх
        {
            int start = 0;

            while (cpu_brand[start] == ' ')
                start++;

            if (start > 0)
            {
                int i = 0;

                while (cpu_brand[start + i])
                {
                    cpu_brand[i] = cpu_brand[start + i];
                    i++;
                }

                cpu_brand[i] = '\0';
            }
        }
    }
    else
    {
        cpu_brand[0] = '\0';
    }
    if (cpu_max_basic_leaf >= 1)
    {
        cpuid(
            1,
            0,
            &eax,
            &ebx,
            &ecx,
            &edx
        );

        cpu_logical_cpus =
            (ebx >> 16) & 0xFF;

        if (cpu_logical_cpus == 0)
            cpu_logical_cpus = 1;
    }
}


/*=============
   RAM
=============*/

static uint64_t ram_available = 0;


#define MULTIBOOT2_TAG_TYPE_END  0
#define MULTIBOOT2_TAG_TYPE_MMAP 6


struct multiboot_tag
{
    uint32_t type;
    uint32_t size;
};


struct multiboot_tag_mmap
{
    uint32_t type;
    uint32_t size;

    uint32_t entry_size;
    uint32_t entry_version;
};


struct multiboot_mmap_entry
{
    uint64_t addr;
    uint64_t len;

    uint32_t type;
    uint32_t reserved;
};


static void detect_ram(uint32_t address)
{
    struct multiboot_tag *tag;

    tag =
        (struct multiboot_tag *)(address + 8);


    while (tag->type != MULTIBOOT2_TAG_TYPE_END)
    {
        if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP)
        {
            struct multiboot_tag_mmap *mmap =
                (struct multiboot_tag_mmap *)tag;


            uint8_t *entry_ptr =
                (uint8_t *)mmap + 16;


            uint8_t *end =
                (uint8_t *)mmap + mmap->size;


            while (entry_ptr < end)
            {
                struct multiboot_mmap_entry *entry =
                    (struct multiboot_mmap_entry *)entry_ptr;


                if (entry->type == 1)
                {
                    ram_available += entry->len;
                }


                entry_ptr +=
                    mmap->entry_size;
            }
        }
        tag =
            (struct multiboot_tag *)
            (((uint32_t)tag + tag->size + 7) & ~7);
    }
}


/* ======================
   FrameBuffer
=======================*/

static uint32_t framebuffer_width = 0;
static uint32_t framebuffer_height = 0;
static uint32_t framebuffer_pitch = 0;
static uint8_t framebuffer_bpp = 0;


#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8


struct multiboot_tag_framebuffer
{
    uint32_t type;
    uint32_t size;

    uint64_t framebuffer_addr;

    uint32_t framebuffer_pitch;

    uint32_t framebuffer_width;
    uint32_t framebuffer_height;

    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;

    uint16_t reserved;
};


static void detect_framebuffer(uint32_t address)
{
    struct multiboot_tag *tag;

    tag =
        (struct multiboot_tag *)(address + 8);


    while (tag->type != MULTIBOOT2_TAG_TYPE_END)
    {
        if (tag->type ==
            MULTIBOOT2_TAG_TYPE_FRAMEBUFFER)
        {
            struct multiboot_tag_framebuffer *fb =
                (struct multiboot_tag_framebuffer *)tag;


            framebuffer_width =
                fb->framebuffer_width;

            framebuffer_height =
                fb->framebuffer_height;

            framebuffer_pitch =
                fb->framebuffer_pitch;

            framebuffer_bpp =
                fb->framebuffer_bpp;

            return;
        }


        tag =
            (struct multiboot_tag *)
            (((uint32_t)tag + tag->size + 7) & ~7);
    }
}


/* ==========================
   Computer name (SMBIOS)
   ==========================
   Реальна назва моделі заліза (те, що показує dmidecode як
   "Product Name") лежить у таблиці SMBIOS, тип структури 1
   (System Information). Шукаємо сигнатуру "_SM_" у діапазоні
   0xF0000-0xFFFFF (стандартне місце для BIOS/SeaBIOS), де вона
   завжди вирівняна на 16 байт.

   Обмеження: підтримується лише SMBIOS 2.x (анкор "_SM_"),
   не SMBIOS 3.0 ("_SM3_"). Якщо таблицю не знайдено (нема BIOS
   SMBIOS, чисте UEFI без CSM тощо) — лишається "Unknown Computer".
========================== */

static char computer_name[64] = "Unknown Computer";


struct smbios_header
{
    uint8_t type;
    uint8_t length;
    uint16_t handle;
};


static const char *smbios_string(struct smbios_header *hdr, uint8_t index)
{
    const char *str;

    if (index == 0)
        return "";

    str = (const char *)hdr + hdr->length;

    while (index > 1 && *str)
    {
        while (*str)
            str++;

        str++;
        index--;
    }

    return str;
}


static void detect_computer_name(void)
{
    uint8_t *p = (uint8_t *)0xF0000;
    uint8_t *scan_end = (uint8_t *)0xFFFF0;
    uint8_t *anchor = 0;

    while (p < scan_end)
    {
        if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_')
        {
            anchor = p;
            break;
        }

        p += 16;
    }

    if (!anchor)
        return;   // SMBIOS не знайдено, лишаємо "Unknown Computer"

    uint32_t table_addr = *(uint32_t *)(anchor + 0x18);
    uint16_t table_len  = *(uint16_t *)(anchor + 0x16);

    uint8_t *table = (uint8_t *)table_addr;
    uint8_t *table_end = table + table_len;

    while (table + sizeof(struct smbios_header) <= table_end)
    {
        struct smbios_header *hdr = (struct smbios_header *)table;

        if (hdr->length < sizeof(struct smbios_header))
            break;   // пошкоджена таблиця — краще зупинитись

        if (hdr->type == 1)   // System Information
        {
            uint8_t product_index = table[5];   // офсет 5 = Product Name

            const char *name = smbios_string(hdr, product_index);

            int i = 0;

            while (name[i] && i < (int)sizeof(computer_name) - 1)
            {
                computer_name[i] = name[i];
                i++;
            }

            computer_name[i] = '\0';

            return;
        }

        if (hdr->type == 127)   // End-of-table
            break;

        uint8_t *strs = table + hdr->length;

        while (!(strs[0] == 0 && strs[1] == 0))
            strs++;

        table = strs + 2;
    }
}


/*==============================
    Init
===============================*/

void hardware_init(uint32_t multiboot_info_addr)
{
    detect_cpu();

    detect_ram(
        multiboot_info_addr
    );

    detect_framebuffer(
        multiboot_info_addr
    );

    detect_computer_name();
}


/*=========================
   Гетери для інших модулів
==========================*/

const char *hardware_get_cpu_vendor(void)
{
    return cpu_vendor;
}

const char *hardware_get_cpu_brand(void)
{
    return cpu_brand[0] ? cpu_brand : "Unknown CPU";
}

int hardware_get_logical_cpus(void)
{
    return cpu_logical_cpus;
}

uint64_t hardware_get_ram_bytes(void)
{
    return ram_available;
}

const char *hardware_get_computer_name(void)
{
    return computer_name;
}


/*=========================
        Output
==========================*/

void hardware_print_info(void)
{
    shell_print("\n");

    shell_print("Hardware Information\n");
    shell_print("====================\n\n");


    //CPU

    shell_print("CPU\n");

    shell_print("  Vendor: ");
    shell_print(cpu_vendor);
    shell_print("\n");

    shell_print("  Model:  ");
    shell_print(cpu_brand);
    shell_print("\n");

    shell_print("  Logical CPUs: ");
    shell_print_int(cpu_logical_cpus);
    shell_print("\n\n");


    //RAM

    shell_print("Memory\n");

    shell_print("  Usable RAM: ");

    shell_print_int(
        (int)(ram_available / (1024 * 1024))
    );

    shell_print(" MB\n\n");


    // Framebuffer

    shell_print("Framebuffer\n");

    shell_print("  Resolution: ");

    shell_print_int(
        (int)framebuffer_width
    );

    shell_print("x");

    shell_print_int(
        (int)framebuffer_height
    );

    shell_print("\n");


    shell_print("  BPP: ");

    shell_print_int(
        (int)framebuffer_bpp
    );

    shell_print("\n");


    shell_print("  Pitch: ");

    shell_print_int(
        (int)framebuffer_pitch
    );

    shell_print(" bytes\n");

    shell_print("\n");
}