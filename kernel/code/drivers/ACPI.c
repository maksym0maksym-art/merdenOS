#include <stdint.h>
#include "../../headers/acpi.h"

/*
 * Вимкнення і перезавантаження, що працюють і у віртуалці (QEMU / VirtualBox / VMware),
 * і на сучасних UEFI-комп'ютерах.
 *
 * Що важливо на справжньому залозі:
 *  - таблиці ACPI на UEFI-машинах НЕ лежать у 0xE0000..0xFFFFF (там порожньо). Вказівник на RSDP
 *    GRUB передає в тегах multiboot2 14/15 — беремо його звідти, скан BIOS-області лишається запасним;
 *  - тип сну SLP_TYP — це біти 10..12 регістра PM1x_CNT (значення з _S5_ треба зсунути на 10);
 *  - перед записом треба переконатися, що ACPI увімкнено (біт SCI_EN), інакше спершу SMI-команда;
 *  - для перезавантаження: ACPI RESET_REG з FADT -> контролер 8042 -> порт 0xCF9 -> потрійний збій.
 */

static inline void io_outb(uint16_t port, uint8_t val) { asm volatile("outb %0, %1" : : "a"(val), "Nd"(port)); }
static inline void io_outw(uint16_t port, uint16_t val) { asm volatile("outw %0, %1" : : "a"(val), "Nd"(port)); }
static inline void io_outl(uint16_t port, uint32_t val) { asm volatile("outl %0, %1" : : "a"(val), "Nd"(port)); }
static inline uint8_t  io_inb(uint16_t port) { uint8_t v;  asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t io_inw(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ~1 мкс на ітерацію (запис у порт 0x80 повільний і на залозі, і у VM) — таймерів тут може ще не бути */
static void delay_us(uint32_t us)
{
    while (us--)
        io_outb(0x80, 0);
}

static void delay_ms(uint32_t ms)
{
    while (ms--)
        delay_us(1000);
}

static uint32_t rd32(const uint8_t *b, uint32_t o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }

#define SDT_LEN_MAX   (16u * 1024u * 1024u)
#define SLP_EN        (1u << 13)
#define SLP_TYP_SHIFT 10
#define SLP_TYP_MASK  (7u << SLP_TYP_SHIFT)
#define SCI_EN        (1u << 0)

/* зміщення полів FADT (ACPI 6.x) */
#define FADT_SMI_CMD      48
#define FADT_ACPI_ENABLE  52
#define FADT_PM1A_CNT     64
#define FADT_PM1B_CNT     68
#define FADT_FLAGS        112
#define FADT_RESET_REG    116   /* GAS, 12 байтів */
#define FADT_RESET_VALUE  128
#define FADT_X_PM1A_CNT   172   /* GAS */
#define FADT_X_PM1B_CNT   184
#define FADT_X_DSDT       140
#define FADT_FLAG_RESET_REG_SUP (1u << 10)

static uint32_t mbi_saved = 0;
static int      inited = 0;
static int      shutdown_ok = 0;

static uint16_t pm1a_cnt_port = 0, pm1b_cnt_port = 0;
static uint16_t smi_cmd_port = 0;
static uint8_t  acpi_enable_val = 0;
static uint8_t  slp_typa = 0, slp_typb = 0;

static int      reset_ok = 0;
static uint8_t  reset_space = 0;     /* 0 — пам'ять, 1 — порт вводу/виводу */
static uint32_t reset_addr = 0;
static uint8_t  reset_value = 0;

static int sig_match(const uint8_t *a, const char *b, int len)
{
    for (int i = 0; i < len; i++)
        if (a[i] != (uint8_t)b[i])
            return 0;
    return 1;
}

static uint8_t sum_bytes(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    while (len--)
        s += *p++;
    return s;
}

/* ---------- пошук RSDP ---------- */

static uint8_t rsdp_buf[36];

static int rsdp_valid(const uint8_t *p)
{
    return sig_match(p, "RSD PTR ", 8) && sum_bytes(p, 20) == 0;
}

/* tag 15 / 14 у multiboot2 містять копію RSDP, яку знайшла прошивка (UEFI та BIOS) */
static const uint8_t *find_rsdp_mbi(uint32_t mbi)
{
    if (!mbi)
        return 0;

    const uint8_t *base = (const uint8_t *)(uintptr_t)mbi;
    uint32_t total = rd32(base, 0);
    uint32_t off = 8;
    const uint8_t *found = 0;

    while (off + 8 <= total)
    {
        uint32_t type = rd32(base, off);
        uint32_t size = rd32(base, off + 4);

        if (type == 0 || size < 8)
            break;

        if ((type == 15 || type == 14) && size >= 8 + 20)
        {
            const uint8_t *p = base + off + 8;
            if (rsdp_valid(p))
            {
                if (type == 15)
                    return p;          /* ACPI 2.0+ — кращий */
                if (!found)
                    found = p;
            }
        }

        off += (size + 7) & ~7u;
    }

    return found;
}

static const uint8_t *find_rsdp_scan(void)
{
    uint16_t ebda_seg = *(volatile uint16_t *)0x40E;
    uint8_t *ebda = (uint8_t *)((uintptr_t)ebda_seg << 4);

    if (ebda_seg >= 0x80)
        for (uint8_t *p = ebda; p < ebda + 1024; p += 16)
            if (rsdp_valid(p))
                return p;

    for (uint8_t *p = (uint8_t *)0xE0000; p < (uint8_t *)0x100000; p += 16)
        if (rsdp_valid(p))
            return p;

    return 0;
}

/* ---------- AML: пакет \_Sx_ = Package { SLP_TYPa, SLP_TYPb, ... } ---------- */

static int parse_sx(const uint8_t *t, uint32_t len, char n, uint8_t *a, uint8_t *b)
{
    for (uint32_t i = 1; i + 16 <= len; i++)
    {
        if (t[i] != '_' || t[i + 1] != 'S' || t[i + 2] != (uint8_t)n || t[i + 3] != '_')
            continue;

        /* перед іменем має бути NameOp (0x08), можливо з кореневим '\' */
        if (!(t[i - 1] == 0x08 || (t[i - 1] == 0x5C && i >= 2 && t[i - 2] == 0x08)))
            continue;

        const uint8_t *q = t + i + 4;
        if (*q != 0x12)                 /* PackageOp */
            continue;
        q++;

        q += 1 + ((*q >> 6) & 3);       /* PkgLength */
        q++;                            /* NumElements */

        uint8_t v[2];
        int ok = 1;

        for (int k = 0; k < 2 && ok; k++)
        {
            if (*q == 0x0A)      { v[k] = q[1]; q += 2; }        /* BytePrefix */
            else if (*q == 0x0B) { v[k] = q[1]; q += 3; }        /* WordPrefix */
            else if (*q == 0x0C) { v[k] = q[1]; q += 5; }        /* DWordPrefix */
            else if (*q <= 0x01) { v[k] = *q;   q += 1; }        /* ZeroOp / OneOp */
            else ok = 0;
        }

        if (!ok)
            continue;

        *a = v[0];
        *b = v[1];
        return 1;
    }

    return 0;
}

static int table_ptr_ok(const uint8_t *h)
{
    return h && (uintptr_t)h >= 0x1000 && rd32(h, 4) >= 36 && rd32(h, 4) <= SDT_LEN_MAX;
}

/* читає адресу з GAS (12 байт: space, width, offset, access, адреса u64); 0 — якщо вона вище 4 ГБ */
static uint32_t gas_addr32(const uint8_t *f, uint32_t off, uint32_t flen, uint8_t *space)
{
    if (off + 12 > flen)
        return 0;
    if (rd32(f, off + 8) != 0)           /* старша половина u64-адреси */
        return 0;
    if (space)
        *space = f[off];
    return rd32(f, off + 4);
}

int acpi_init(void)
{
    if (inited)
        return shutdown_ok;
    inited = 1;

    const uint8_t *src = find_rsdp_mbi(mbi_saved);
    if (!src)
        src = find_rsdp_scan();
    if (!src)
        return 0;

    uint32_t rlen = (src[15] >= 2) ? 36 : 20;      /* ревізія 2+ — 36 байт */
    for (uint32_t i = 0; i < rlen; i++)
        rsdp_buf[i] = src[i];

    const uint8_t *rsdp = rsdp_buf;
    const uint8_t *root = 0;
    uint32_t esz = 4;

    if (rsdp[15] >= 2 && rd32(rsdp, 20) >= 36 && sum_bytes(rsdp, 36) == 0 &&
        rd32(rsdp, 28) == 0 && rd32(rsdp, 24) != 0)
    {
        const uint8_t *x = (const uint8_t *)(uintptr_t)rd32(rsdp, 24);
        if (table_ptr_ok(x) && sig_match(x, "XSDT", 4))
        {
            root = x;
            esz = 8;
        }
    }

    if (!root)
    {
        const uint8_t *r = (const uint8_t *)(uintptr_t)rd32(rsdp, 16);
        if (table_ptr_ok(r) && sig_match(r, "RSDT", 4))
            root = r;
    }

    if (!root)
        return 0;

    uint32_t rootlen = rd32(root, 4);
    uint32_t entries = (rootlen - 36) / esz;

    const uint8_t *fadt = 0;
    const uint8_t *ssdt[16];
    int nssdt = 0;

    for (uint32_t i = 0; i < entries; i++)
    {
        uint32_t lo = rd32(root, 36 + i * esz);
        if (esz == 8 && rd32(root, 36 + i * esz + 4) != 0)
            continue;                    /* таблиця вище 4 ГБ — з 32-бітного ядра недосяжна */

        const uint8_t *h = (const uint8_t *)(uintptr_t)lo;
        if (!table_ptr_ok(h))
            continue;

        if (sig_match(h, "FACP", 4))
            fadt = h;
        else if (sig_match(h, "SSDT", 4) && nssdt < 16)
            ssdt[nssdt++] = h;
    }

    if (!fadt)
        return 0;

    uint32_t flen = rd32(fadt, 4);

    smi_cmd_port    = (uint16_t)rd32(fadt, FADT_SMI_CMD);
    acpi_enable_val = fadt[FADT_ACPI_ENABLE];

    uint32_t pa = rd32(fadt, FADT_PM1A_CNT);
    uint32_t pb = rd32(fadt, FADT_PM1B_CNT);
    uint8_t sp;

    if (!pa) { sp = 1; pa = gas_addr32(fadt, FADT_X_PM1A_CNT, flen, &sp); if (sp != 1) pa = 0; }
    if (!pb) { sp = 1; pb = gas_addr32(fadt, FADT_X_PM1B_CNT, flen, &sp); if (sp != 1) pb = 0; }

    pm1a_cnt_port = (uint16_t)pa;
    pm1b_cnt_port = (uint16_t)pb;

    /* RESET_REG (ACPI 2.0+) */
    if (flen >= FADT_RESET_VALUE + 1 && (rd32(fadt, FADT_FLAGS) & FADT_FLAG_RESET_REG_SUP))
    {
        uint8_t space = 0xFF;
        uint32_t a = gas_addr32(fadt, FADT_RESET_REG, flen, &space);

        if (a && (space == 0 || space == 1) && !(space == 1 && a > 0xFFFF))
        {
            reset_ok = 1;
            reset_space = space;
            reset_addr = a;
            reset_value = fadt[FADT_RESET_VALUE];
        }
    }

    /* \_S5_ шукаємо в DSDT, а якщо там немає — в SSDT */
    uint32_t dsdt_addr = rd32(fadt, 40);
    if (!dsdt_addr && flen >= FADT_X_DSDT + 8 && rd32(fadt, FADT_X_DSDT + 4) == 0)
        dsdt_addr = rd32(fadt, FADT_X_DSDT);

    int have_s5 = 0;
    const uint8_t *dsdt = (const uint8_t *)(uintptr_t)dsdt_addr;

    if (table_ptr_ok(dsdt) && sig_match(dsdt, "DSDT", 4))
        have_s5 = parse_sx(dsdt, rd32(dsdt, 4), '5', &slp_typa, &slp_typb);

    for (int i = 0; i < nssdt && !have_s5; i++)
        have_s5 = parse_sx(ssdt[i], rd32(ssdt[i], 4), '5', &slp_typa, &slp_typb);

    shutdown_ok = have_s5 && pm1a_cnt_port;
    return shutdown_ok;
}

int acpi_init_mbi(uint32_t mbi)
{
    mbi_saved = mbi;
    inited = 0;
    return acpi_init();
}

/* ---------- вимкнення ---------- */

static void halt_forever(void)
{
    asm volatile("cli");
    for (;;)
        asm volatile("hlt");
}

static int sci_enabled(void)
{
    return (io_inw(pm1a_cnt_port) & SCI_EN) != 0;
}

void acpi_shutdown(void)
{
    asm volatile("cli");
    acpi_init();

    if (shutdown_ok)
    {
        /* якщо ACPI ще не ввімкнено (немає SCI_EN) — просимо прошивку через SMI */
        if (!sci_enabled() && smi_cmd_port && acpi_enable_val)
        {
            io_outb(smi_cmd_port, acpi_enable_val);
            for (int i = 0; i < 3000 && !sci_enabled(); i++)
                delay_ms(1);
        }

        uint16_t a = io_inw(pm1a_cnt_port) & ~(uint16_t)SLP_TYP_MASK;
        uint16_t b = pm1b_cnt_port ? (io_inw(pm1b_cnt_port) & ~(uint16_t)SLP_TYP_MASK) : 0;

        a |= (uint16_t)((slp_typa & 7) << SLP_TYP_SHIFT);
        b |= (uint16_t)((slp_typb & 7) << SLP_TYP_SHIFT);

        /* спершу тип сну, потім SLP_EN — так робить і Linux */
        io_outw(pm1a_cnt_port, a);
        if (pm1b_cnt_port)
            io_outw(pm1b_cnt_port, b);

        io_outw(pm1a_cnt_port, a | SLP_EN);
        if (pm1b_cnt_port)
            io_outw(pm1b_cnt_port, b | SLP_EN);

        delay_ms(1000);                  /* залізо вимикається не миттєво */
    }

    /* запасні порти емуляторів */
    io_outw(0x604,  0x2000);             /* QEMU (q35 / нові версії) */
    io_outw(0xB004, 0x2000);             /* Bochs / старий QEMU */
    io_outw(0x4004, 0x3400);             /* VirtualBox */
    delay_ms(500);

    halt_forever();
}

/* ---------- перезавантаження ---------- */

static void kbc_wait_input_empty(void)
{
    for (int i = 0; i < 100000; i++)
    {
        if (!(io_inb(0x64) & 2))
            return;
        io_outb(0x80, 0);
    }
}

void acpi_reboot(void)
{
    asm volatile("cli");
    acpi_init();

    /* 1. ACPI RESET_REG з FADT (на сучасних UEFI-платах зазвичай це порт 0xCF9) */
    if (reset_ok)
    {
        if (reset_space == 1)
            io_outb((uint16_t)reset_addr, reset_value);
        else
            *(volatile uint8_t *)(uintptr_t)reset_addr = reset_value;

        delay_ms(250);
    }

    /* 2. контролер клавіатури 8042: імпульс на лінію reset (QEMU, VirtualBox, старі ПК) */
    kbc_wait_input_empty();
    io_outb(0x64, 0xFE);
    delay_ms(250);

    /* 3. порт 0xCF9 чипсета: спершу м'який, потім повний скид */
    io_outb(0xCF9, 0x02);
    delay_us(50);
    io_outb(0xCF9, 0x06);
    delay_ms(250);
    io_outb(0xCF9, 0x0E);
    delay_ms(250);

    /* 4. потрійний збій: порожня IDT + виняток */
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idt = { 0, 0 };
    asm volatile("lidt %0; int3" : : "m"(idt));

    halt_forever();
}
