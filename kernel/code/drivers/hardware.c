#include "../../headers/hardware.h"
#include "../../headers/framebuffer.h"   /* framebuffer_* — один спільний набір змінних, а не своя копія */

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

        /* EBX[23:16] — це МАКСИМУМ логічних ID в пакеті, а не реальна кількість
           ядер (на i3 з 2C/4T тут буде 16). Реальну кількість рахуємо з ACPI MADT
           (detect_cpu_count), а це — лише запасне значення. */
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

/* framebuffer_width/height/pitch/bpp беруться з framebuffer.h (ставить frambuffer.c) */


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


            (void)fb;                   /* дані вже в frambuffer.c */

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


/* Реальний встановлений обсяг RAM = сума розмірів усіх планок із SMBIOS (тип 17,
   "Memory Device"). Це те, що фізично стоїть у слотах, а не "usable" з мапи пам'яті
   (яка не враховує діри під PCI, пам'ять під вбудоване відео тощо). */
static uint32_t smbios_ram_mb = 0;

static void ram_from_table(uint8_t *table, uint32_t table_len)
{
    uint8_t *table_end = table + table_len;
    uint32_t total = 0;

    while (table + sizeof(struct smbios_header) <= table_end)
    {
        struct smbios_header *hdr = (struct smbios_header *)table;

        if (hdr->length < sizeof(struct smbios_header))
            break;

        if (hdr->type == 17 && hdr->length >= 0x0E)   // Memory Device (одна планка)
        {
            uint16_t size = *(uint16_t *)(table + 0x0C);
            uint32_t mb = 0;

            if (size == 0x7FFF && hdr->length >= 0x20)
                mb = *(uint32_t *)(table + 0x1C) & 0x7FFFFFFF;   // розширений розмір, МБ
            else if (size == 0 || size == 0xFFFF)
                mb = 0;                                           // слот порожній / невідомо
            else if (size & 0x8000)
                mb = (size & 0x7FFF) / 1024;                      // розмір у КБ
            else
                mb = size;                                        // розмір у МБ

            total += mb;
        }

        if (hdr->type == 127)   // End-of-table
            break;

        uint8_t *strs = table + hdr->length;

        while (strs + 1 < table_end && !(strs[0] == 0 && strs[1] == 0))
            strs++;

        table = strs + 2;
    }

    smbios_ram_mb = total;   // присвоєння, а не +=, щоб повторний виклик не подвоював суму
}


static int name_from_table(uint8_t *table, uint32_t table_len)
{
    uint8_t *table_end = table + table_len;

    while (table + sizeof(struct smbios_header) <= table_end)
    {
        struct smbios_header *hdr = (struct smbios_header *)table;

        if (hdr->length < sizeof(struct smbios_header))
            break;   // пошкоджена таблиця — краще зупинитись

        if (hdr->type == 1 && hdr->length > 5)   // System Information
        {
            const char *name = smbios_string(hdr, table[5]);   // офсет 5 = Product Name
            int i = 0;

            while (name[i] && i < (int)sizeof(computer_name) - 1)
            {
                computer_name[i] = name[i];
                i++;
            }

            computer_name[i] = '\0';

            return i > 0;
        }

        if (hdr->type == 127)   // End-of-table
            break;

        uint8_t *strs = table + hdr->length;

        while (strs + 1 < table_end && !(strs[0] == 0 && strs[1] == 0))
            strs++;

        table = strs + 2;
    }

    return 0;
}


static int try_smbios_anchor(uint8_t *anchor)
{
    if (anchor[0] == '_' && anchor[1] == 'S' && anchor[2] == 'M' && anchor[3] == '3' && anchor[4] == '_')
    {
        /* SMBIOS 3.0: максимальний розмір таблиці @0x0C, 64-бітна адреса @0x10 */
        uint32_t len = *(uint32_t *)(anchor + 0x0C);
        uint32_t lo  = *(uint32_t *)(anchor + 0x10);
        uint32_t hi  = *(uint32_t *)(anchor + 0x14);

        if (hi != 0 || lo == 0)
            return 0;   // таблиця вище 4 ГБ — 32-бітне ядро її не бачить

        ram_from_table((uint8_t *)lo, len);
        return name_from_table((uint8_t *)lo, len);
    }

    if (anchor[0] == '_' && anchor[1] == 'S' && anchor[2] == 'M' && anchor[3] == '_')
    {
        uint32_t table_addr = *(uint32_t *)(anchor + 0x18);
        uint16_t table_len  = *(uint16_t *)(anchor + 0x16);

        if (table_addr == 0)
            return 0;

        ram_from_table((uint8_t *)table_addr, table_len);
        return name_from_table((uint8_t *)table_addr, table_len);
    }

    return 0;
}


static int guid_eq(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;

    return 1;
}


static struct multiboot_tag *find_mb_tag(uint32_t address, uint32_t type)
{
    struct multiboot_tag *tag = (struct multiboot_tag *)(address + 8);

    while (tag->type != MULTIBOOT2_TAG_TYPE_END)
    {
        if (tag->type == type)
            return tag;

        tag = (struct multiboot_tag *)(((uint32_t)tag + tag->size + 7) & ~7);
    }

    return 0;
}


/* На сучасних UEFI-ноутбуках (Vivobook та ін.) таблиці SMBIOS/ACPI НЕ лежать у
   0xF0000..0xFFFFF — їх адреси є в EFI Configuration Table. GRUB передає
   вказівник на EFI System Table тегами multiboot2 11 (32-біт) та 12 (64-біт). */
static int detect_name_efi(uint32_t mbi)
{
    static const uint8_t guid_smbios3[16] = { 0x44,0x15,0xfd,0xf2, 0x94,0x97, 0x2c,0x4a, 0x99,0x2e, 0xe5,0xbb,0xcf,0x20,0xe3,0x94 };
    static const uint8_t guid_smbios2[16] = { 0x31,0x2d,0x9d,0xeb, 0x88,0x2d, 0xd3,0x11, 0x9a,0x16, 0x00,0x90,0x27,0x3f,0xc1,0x4d };

    struct multiboot_tag *t64 = find_mb_tag(mbi, 12);
    struct multiboot_tag *t32 = find_mb_tag(mbi, 11);
    uint8_t *cfg = 0;
    uint32_t count = 0, esz = 0, vend_off = 16;

    if (t64)
    {
        uint32_t lo = ((uint32_t *)t64)[2], hi = ((uint32_t *)t64)[3];

        if (hi != 0 || lo == 0)
            return 0;

        uint8_t *st = (uint8_t *)lo;

        count = *(uint32_t *)(st + 0x68);
        uint32_t cl = *(uint32_t *)(st + 0x70), ch = *(uint32_t *)(st + 0x74);

        if (ch != 0)
            return 0;

        cfg = (uint8_t *)cl;
        esz = 24;
    }
    else if (t32)
    {
        uint8_t *st = (uint8_t *)((uint32_t *)t32)[2];

        if (!st)
            return 0;

        count = *(uint32_t *)(st + 64);
        cfg = (uint8_t *)*(uint32_t *)(st + 68);
        esz = 20;
    }
    else
        return 0;

    if (!cfg || count > 256)
        return 0;

    for (int pass = 0; pass < 2; pass++)   // спочатку SMBIOS 3, потім 2
    {
        const uint8_t *want = pass == 0 ? guid_smbios3 : guid_smbios2;

        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t *e = cfg + i * esz;

            if (!guid_eq(e, want))
                continue;

            uint32_t lo = *(uint32_t *)(e + vend_off);
            uint32_t hi = esz == 24 ? *(uint32_t *)(e + vend_off + 4) : 0;

            if (hi == 0 && lo != 0 && try_smbios_anchor((uint8_t *)lo))
                return 1;
        }
    }

    return 0;
}


static void detect_computer_name(uint32_t mbi)
{
    if (detect_name_efi(mbi))
        return;

    /* BIOS/CSM: анкор лежить у 0xF0000..0xFFFFF, вирівняний на 16 байт */
    for (uint8_t *p = (uint8_t *)0xF0000; p < (uint8_t *)0xFFFF0; p += 16)
        if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' && try_smbios_anchor(p))
            return;
}


/* ==========================
   Кількість логічних CPU (ACPI MADT)
========================== */

struct acpi_sdt { char sig[4]; uint32_t length; uint8_t rev, csum; char oem[6]; char oemt[8]; uint32_t oemrev, cid, crev; } __attribute__((packed));

static int sig4(const char *a, const char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int sum_ok(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;

    while (n--)
        s += *p++;

    return s == 0;
}

static void detect_cpu_count(uint32_t mbi)
{
    uint8_t *rsdp = 0;
    struct multiboot_tag *t = find_mb_tag(mbi, 15);

    if (!t)
        t = find_mb_tag(mbi, 14);

    if (t)
        rsdp = (uint8_t *)t + 8;
    else
        for (uint8_t *p = (uint8_t *)0xE0000; p < (uint8_t *)0x100000; p += 16)
            if (p[0]=='R' && p[1]=='S' && p[2]=='D' && p[3]==' ' && p[4]=='P' && p[5]=='T' && p[6]=='R' && p[7]==' ' && sum_ok(p, 20))
            {
                rsdp = p;
                break;
            }

    if (!rsdp)
        return;

    struct acpi_sdt *rsdt = (struct acpi_sdt *)*(uint32_t *)(rsdp + 16);

    if (!rsdt || rsdt->length < sizeof(struct acpi_sdt) || rsdt->length > 0x10000)
        return;

    uint32_t n = (rsdt->length - sizeof(struct acpi_sdt)) / 4;
    uint32_t *ent = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt));

    for (uint32_t i = 0; i < n; i++)
    {
        struct acpi_sdt *h = (struct acpi_sdt *)ent[i];

        if (!h || !sig4(h->sig, "APIC") || h->length < 44 || h->length > 0x10000)
            continue;

        uint8_t *q = (uint8_t *)h + 44, *end = (uint8_t *)h + h->length;
        int cpus = 0;

        while (q + 2 <= end && q[1] >= 2)
        {
            if (q[0] == 0 && q[1] >= 8)                       // Processor Local APIC
            {
                if (q[4] & 3) cpus++;                          // enabled | online-capable
            }
            else if (q[0] == 9 && q[1] >= 16)                 // Processor Local x2APIC
            {
                if (q[8] & 3) cpus++;
            }

            q += q[1];
        }

        if (cpus > 0)
            cpu_logical_cpus = cpus;

        return;
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

    detect_computer_name(multiboot_info_addr);

    detect_cpu_count(multiboot_info_addr);
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

/* Встановлена RAM (сума планок із SMBIOS). Якщо SMBIOS недоступний — usable з мапи пам'яті */
uint64_t hardware_get_ram_installed_bytes(void)
{
    if (smbios_ram_mb > 0)
        return (uint64_t)smbios_ram_mb * 1024 * 1024;

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

    shell_print("  Installed RAM: ");

    shell_print_int(
        (int)(hardware_get_ram_installed_bytes() / (1024 * 1024))
    );

    shell_print(" MB\n");

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