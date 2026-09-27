#include <stdint.h>
#include "../../headers/acpi.h"


static inline void io_outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

typedef struct
{
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed)) rsdp_t;

typedef struct
{
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

typedef struct
{
    sdt_header_t header;

    uint32_t firmware_ctrl;
    uint32_t dsdt;

    uint8_t  reserved1;

    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;

    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;

    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;

    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
} __attribute__((packed)) fadt_t;

#define SLP_EN (1 << 13)

static uint32_t pm1a_cnt_port   = 0;
static uint32_t pm1b_cnt_port   = 0;
static uint32_t smi_cmd_port    = 0;
static uint8_t  acpi_enable_val = 0;

static uint16_t slp_typa = 0;
static uint16_t slp_typb = 0;
static int      acpi_ready = 0;

static uint8_t sum_bytes(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;

    for (uint32_t i = 0; i < len; i++)
        sum += p[i];

    return sum;
}

static int sig_match(const char *a, const char *b, int len)
{
    for (int i = 0; i < len; i++)
        if (a[i] != b[i])
            return 0;

    return 1;
}

static rsdp_t *find_rsdp(void)
{
    uint16_t ebda_seg = *(uint16_t *)0x40E;
    uint8_t *ebda = (uint8_t *)((uint32_t)ebda_seg << 4);

    for (uint8_t *p = ebda; p < ebda + 1024; p += 16)
    {
        if (sig_match((char *)p, "RSD PTR ", 8) &&
            sum_bytes(p, sizeof(rsdp_t)) == 0)
            return (rsdp_t *)p;
    }

    for (uint8_t *p = (uint8_t *)0xE0000; p < (uint8_t *)0x100000; p += 16)
    {
        if (sig_match((char *)p, "RSD PTR ", 8) &&
            sum_bytes(p, sizeof(rsdp_t)) == 0)
            return (rsdp_t *)p;
    }

    return 0;
}

static int parse_s5(sdt_header_t *dsdt)
{
    uint8_t *p   = (uint8_t *)dsdt;
    uint8_t *end = p + dsdt->length - 4;

    for (uint8_t *s = p; s < end; s++)
    {
        if (s[0] == '_' && s[1] == 'S' && s[2] == '5' && s[3] == '_')
        {
            uint8_t *q = s + 4;

            if (*q == 0x12)
                q++;

            int extra_bytes = (*q >> 6) & 0x3;
            q += 1 + extra_bytes;

            q++;

            uint16_t values[2] = { 0, 0 };

            for (int i = 0; i < 2; i++)
            {
                if (*q == 0x0A)
                {
                    q++;
                    values[i] = *q++;
                }
                else if (*q <= 0x09)
                {
                    values[i] = *q++;
                }
                else
                {
                    return 0;
                }
            }

            slp_typa = values[0];
            slp_typb = values[1];
            return 1;
        }
    }

    return 0;
}

int acpi_init(void)
{
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp)
        return 0;

    sdt_header_t *rsdt = (sdt_header_t *)rsdp->rsdt_address;
    if (!sig_match(rsdt->signature, "RSDT", 4))
        return 0;
    if (sum_bytes((uint8_t *)rsdt, rsdt->length) != 0)
        return 0;

    int entries = (rsdt->length - sizeof(sdt_header_t)) / 4;
    uint32_t *entry_ptr = (uint32_t *)((uint8_t *)rsdt + sizeof(sdt_header_t));

    fadt_t *fadt = 0;

    for (int i = 0; i < entries; i++)
    {
        sdt_header_t *h = (sdt_header_t *)entry_ptr[i];

        if (sig_match(h->signature, "FACP", 4))
        {
            fadt = (fadt_t *)h;
            break;
        }
    }

    if (!fadt)
        return 0;

    pm1a_cnt_port   = fadt->pm1a_control_block;
    pm1b_cnt_port   = fadt->pm1b_control_block;
    smi_cmd_port    = fadt->smi_command_port;
    acpi_enable_val = fadt->acpi_enable;

    sdt_header_t *dsdt = (sdt_header_t *)fadt->dsdt;

    if (!sig_match(dsdt->signature, "DSDT", 4))
        return 0;

    if (!parse_s5(dsdt))
        return 0;

    acpi_ready = 1;
    return 1;
}

void acpi_shutdown(void)
{
    if (!acpi_ready)
        return;

    if (smi_cmd_port && acpi_enable_val)
    {
        io_outb((uint16_t)smi_cmd_port, acpi_enable_val);

        for (volatile int i = 0; i < 1000000; i++)
            ;
    }

    io_outw((uint16_t)pm1a_cnt_port, slp_typa | SLP_EN);

    if (pm1b_cnt_port)
        io_outw((uint16_t)pm1b_cnt_port, slp_typb | SLP_EN);
}