/*
 * Legacy IDE/ATA, PIO, LBA28. Див. опис і обмеження в headers/IDE_drive.h.
 * Збірка для хост-тесту (-DIDE_HOST_TEST): порти й insw/outsw підміняються на ide_hw_*.
 */
#include <stdint.h>
#include "../../headers/IDE_drive.h"

#ifdef IDE_HOST_TEST
extern uint8_t ide_hw_inb(uint16_t port);
extern void    ide_hw_outb(uint16_t port, uint8_t v);
extern void    ide_hw_insw(uint16_t port, void *buf, uint32_t words);
extern void    ide_hw_outsw(uint16_t port, const void *buf, uint32_t words);
#define HW_INB(p)          ide_hw_inb(p)
#define HW_OUTB(p, v)      ide_hw_outb((p), (v))
#define HW_INSW(p, b, n)   ide_hw_insw((p), (b), (n))
#define HW_OUTSW(p, b, n)  ide_hw_outsw((p), (b), (n))
#else
static inline uint8_t hw_inb(uint16_t port)
{
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void hw_outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void hw_insw(uint16_t port, void *addr, uint32_t count)
{
    __asm__ volatile ("cld\n\trep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}
static inline void hw_outsw(uint16_t port, const void *addr, uint32_t count)
{
    __asm__ volatile ("cld\n\trep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}
#define HW_INB(p)          hw_inb(p)
#define HW_OUTB(p, v)      hw_outb((p), (v))
#define HW_INSW(p, b, n)   hw_insw((p), (b), (n))
#define HW_OUTSW(p, b, n)  hw_outsw((p), (b), (n))
#endif

/* статус */
#define ATA_SR_BSY   0x80
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

/* команди */
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_IDENTIFY        0xEC

/* зсуви в 512-байтовій відповіді IDENTIFY (у байтах) */
#define ATA_IDENT_DEVICETYPE   0
#define ATA_IDENT_MODEL        54
#define ATA_IDENT_CAPABILITIES 98
#define ATA_IDENT_MAX_LBA      120     /* слова 60-61: секторів, доступних через LBA28 */
#define ATA_IDENT_COMMANDSETS  164

/* регістри командного блоку (зсув від base) */
#define ATA_REG_DATA       0x00
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07

#define ATA_PRIMARY    0x00
#define ATA_SECONDARY  0x01
#define ATA_READ       0x00
#define ATA_WRITE      0x01

#define IDE_POLL_ITERS      1000000u   /* очікування BSY/DRQ під час обміну даними (~1 с на залізі) */
#define IDE_IDENT_ITERS     100000u    /* під час пошуку дисків — коротше, щоб порожні канали не гальмували */
#define IDE_CHUNK_SECTORS   128u       /* за один виклик команди; регістр лічильника 8-бітний */
#define IDE_LBA28_LIMIT     0x10000000u

struct IDEChannelRegisters channels[2];

unsigned char ide_buf[2048] = {0};
unsigned char atapi_packet[12] = {0xA8, 0,0,0,0,0,0,0,0,0,0,0};
struct ide_devices_s ide_devices[IDE_MAX_DRIVES];
unsigned char package[2] = {0, 0};

static blockdev_t ide_bd[IDE_MAX_DRIVES];

/* ---------- низький рівень ---------- */

static inline void io_delay(void)
{
    HW_OUTB(0x80, 0);
}

static inline uint8_t reg_in(int ch, int reg)
{
    return HW_INB((uint16_t)(channels[ch].base + reg));
}

static inline void reg_out(int ch, int reg, uint8_t v)
{
    HW_OUTB((uint16_t)(channels[ch].base + reg), v);
}

/* Alternate Status: те саме, що Status, але читання не гасить IRQ і безпечне для затримок */
static inline uint8_t alt_status(int ch)
{
    return HW_INB((uint16_t)channels[ch].ctrl);
}

static inline void ctrl_out(int ch, uint8_t v)
{
    HW_OUTB((uint16_t)channels[ch].ctrl, v);
}

/* 400 нс після вибору диска / видачі команди: чотири читання Alt Status */
static void wait_400ns(int ch)
{
    for (int i = 0; i < 4; i++)
        (void)alt_status(ch);
}

static uint16_t rd16(const unsigned char *b, int off)
{
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}

static uint32_t rd32(const unsigned char *b, int off)
{
    return (uint32_t)rd16(b, off) | ((uint32_t)rd16(b, off + 2) << 16);
}

/* чекає BSY=0, потім (за потреби) DRQ=1 */
static int ide_poll(int ch, int check_drq)
{
    wait_400ns(ch);

    uint32_t t = 0;
    uint8_t st;

    while ((st = alt_status(ch)) & ATA_SR_BSY)
        if (++t > IDE_POLL_ITERS)
            return IDE_E_TIMEOUT;

    if (st & ATA_SR_ERR) return IDE_E_ERR;
    if (st & ATA_SR_DF)  return IDE_E_DF;

    if (check_drq) {
        t = 0;
        while (!((st = alt_status(ch)) & ATA_SR_DRQ)) {
            if (st & ATA_SR_ERR) return IDE_E_ERR;
            if (st & ATA_SR_DF)  return IDE_E_DF;
            if (++t > IDE_POLL_ITERS) return IDE_E_TIMEOUT;
        }
    }
    return IDE_OK;
}

static int ide_wait_not_busy(int ch)
{
    uint32_t t = 0;
    while (alt_status(ch) & ATA_SR_BSY)
        if (++t > IDE_POLL_ITERS)
            return IDE_E_TIMEOUT;
    return IDE_OK;
}

/* ---------- пошук дисків ---------- */

static int ide_identify_device(int ch, int drv, int *out_type)
{
    reg_out(ch, ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (drv << 4)));
    io_delay();
    wait_400ns(ch);

    uint8_t status = reg_in(ch, ATA_REG_STATUS);
    if (status == 0xFF)
        return 0;                       /* плаваюча шина: нічого не підключено */

    reg_out(ch, ATA_REG_SECCOUNT0, 0);
    reg_out(ch, ATA_REG_LBA0, 0);
    reg_out(ch, ATA_REG_LBA1, 0);
    reg_out(ch, ATA_REG_LBA2, 0);
    reg_out(ch, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_delay();

    status = reg_in(ch, ATA_REG_STATUS);
    if (status == 0)
        return 0;                       /* пристрою нема */

    uint32_t timeout = 0;
    int type = IDE_ATA;

    for (;;) {
        status = reg_in(ch, ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            uint8_t cl = reg_in(ch, ATA_REG_LBA1);
            uint8_t chh = reg_in(ch, ATA_REG_LBA2);

            if ((cl == 0x14 && chh == 0xEB) || (cl == 0x69 && chh == 0x96)) {
                type = IDE_ATAPI;
                reg_out(ch, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
                io_delay();
                timeout = 0;
                while (!((status = reg_in(ch, ATA_REG_STATUS)) & ATA_SR_DRQ)) {
                    if (status & ATA_SR_ERR) return 0;
                    if (++timeout > IDE_IDENT_ITERS) return 0;
                }
                break;
            }
            return 0;
        }

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            break;
        if (++timeout > IDE_IDENT_ITERS)
            return 0;
    }

    HW_INSW((uint16_t)(channels[ch].base + ATA_REG_DATA), ide_buf, 256);
    *out_type = type;
    return 1;
}

void ide_init(unsigned int BAR0, unsigned int BAR1,
              unsigned int BAR2, unsigned int BAR3,
              unsigned int BAR4)
{
    /* BAR = 0 — класичні порти. У PCI-native BAR1/BAR3 вказують на блок із 4 портів,
     * а Device Control/Alt Status лежить по зсуву +2. */
    channels[ATA_PRIMARY].base   = BAR0 ? (BAR0 & 0xFFFFFFFCu) : 0x1F0;
    channels[ATA_PRIMARY].ctrl   = BAR1 ? (BAR1 & 0xFFFFFFFCu) + 2 : 0x3F6;
    channels[ATA_SECONDARY].base = BAR2 ? (BAR2 & 0xFFFFFFFCu) : 0x170;
    channels[ATA_SECONDARY].ctrl = BAR3 ? (BAR3 & 0xFFFFFFFCu) + 2 : 0x376;
    channels[ATA_PRIMARY].bmide   = BAR4 & 0xFFFFFFFCu;
    channels[ATA_SECONDARY].bmide = (BAR4 & 0xFFFFFFFCu) + 8;

    /* nIEN = 1: працюємо поллінгом, IRQ14/15 не потрібні */
    channels[ATA_PRIMARY].nIEN   = 2;
    channels[ATA_SECONDARY].nIEN = 2;
    ctrl_out(ATA_PRIMARY,   2);
    ctrl_out(ATA_SECONDARY, 2);

    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        ide_devices[i].Reserved = 0;
        ide_bd[i].ctx = 0;
    }

    int count = 0;
    for (int ch = 0; ch < 2; ch++) {
        for (int drv = 0; drv < 2; drv++) {
            int type = IDE_ATA;

            if (!ide_identify_device(ch, drv, &type))
                continue;

            struct ide_devices_s *d = &ide_devices[count];
            d->Reserved     = 1;
            d->Type         = (unsigned short)type;
            d->channel      = (unsigned char)ch;
            d->Drive        = (unsigned char)drv;
            d->Signature    = rd16(ide_buf, ATA_IDENT_DEVICETYPE);
            d->Capabilities = rd16(ide_buf, ATA_IDENT_CAPABILITIES);
            d->CommandSets  = rd32(ide_buf, ATA_IDENT_COMMANDSETS);
            d->Size         = rd32(ide_buf, ATA_IDENT_MAX_LBA);

            for (int k = 0; k < 40; k += 2) {
                d->Model[k]     = ide_buf[ATA_IDENT_MODEL + k + 1];
                d->Model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
            }
            d->Model[40] = 0;
            for (int k = 39; k >= 0 && (d->Model[k] == ' ' || d->Model[k] == 0); k--)
                d->Model[k] = 0;

            if (++count == IDE_MAX_DRIVES)
                return;
        }
    }
}

int ide_drive_count(void)
{
    int n = 0;
    for (int i = 0; i < IDE_MAX_DRIVES; i++)
        if (ide_devices[i].Reserved)
            n++;
    return n;
}

const char *ide_strerror(int code)
{
    switch (code) {
    case IDE_OK:        return "ok";
    case IDE_E_DF:      return "device fault";
    case IDE_E_ERR:     return "drive reported an error";
    case IDE_E_NODRIVE: return "no such drive";
    case IDE_E_TIMEOUT: return "timeout";
    case IDE_E_RANGE:   return "LBA out of range";
    case IDE_E_NOLBA:   return "drive has no LBA support";
    case IDE_E_NOTATA:  return "not an ATA disk (ATAPI)";
    default:            return "unknown error";
    }
}

/* ---------- обмін даними ---------- */

static int check_drive(int drive, uint32_t lba, uint32_t count)
{
    if (drive < 0 || drive >= IDE_MAX_DRIVES || !ide_devices[drive].Reserved)
        return IDE_E_NODRIVE;
    if (ide_devices[drive].Type != IDE_ATA)
        return IDE_E_NOTATA;
    if (!(ide_devices[drive].Capabilities & 0x200))
        return IDE_E_NOLBA;
    if ((uint64_t)lba + count > ide_devices[drive].Size ||
        (uint64_t)lba + count > IDE_LBA28_LIMIT)
        return IDE_E_RANGE;
    return IDE_OK;
}

static int select_drive(int ch, int slave, uint32_t lba_top4)
{
    int e = ide_wait_not_busy(ch);
    if (e) return e;
    reg_out(ch, ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (slave << 4) | (lba_top4 & 0x0F)));
    io_delay();
    wait_400ns(ch);
    return ide_wait_not_busy(ch);
}

/* одна команда READ/WRITE SECTORS на 1..128 секторів; межі вже перевірені */
static int ata_pio(int direction, int drive, uint32_t lba, uint32_t n, void *buf)
{
    int ch    = ide_devices[drive].channel;
    int slave = ide_devices[drive].Drive;
    uint16_t data_port = (uint16_t)(channels[ch].base + ATA_REG_DATA);
    uint8_t *p = (uint8_t *)buf;

    int e = select_drive(ch, slave, lba >> 24);
    if (e) return e;

    reg_out(ch, ATA_REG_SECCOUNT0, (uint8_t)n);
    reg_out(ch, ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    reg_out(ch, ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    reg_out(ch, ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    reg_out(ch, ATA_REG_COMMAND, direction == ATA_READ ? ATA_CMD_READ_PIO : ATA_CMD_WRITE_PIO);

    for (uint32_t i = 0; i < n; i++) {
        e = ide_poll(ch, 1);
        if (e) return e;
        if (direction == ATA_READ)
            HW_INSW(data_port, p + i * IDE_SECTOR_SIZE, IDE_SECTOR_SIZE / 2);
        else
            HW_OUTSW(data_port, p + i * IDE_SECTOR_SIZE, IDE_SECTOR_SIZE / 2);
    }

    if (direction == ATA_WRITE)
        return ide_poll(ch, 0);         /* диск закінчує запис останнього сектора (BSY) і може видати ERR */
    return IDE_OK;
}

int ide_read_blocks(int drive, uint32_t lba, uint32_t count, void *buf)
{
    int e = check_drive(drive, lba, count);
    if (e) return e;

    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > IDE_CHUNK_SECTORS ? IDE_CHUNK_SECTORS : count;
        e = ata_pio(ATA_READ, drive, lba, n, p);
        if (e) return e;
        lba += n; count -= n; p += n * IDE_SECTOR_SIZE;
    }
    return IDE_OK;
}

int ide_write_blocks(int drive, uint32_t lba, uint32_t count, const void *buf)
{
    int e = check_drive(drive, lba, count);
    if (e) return e;

    const uint8_t *p = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count > IDE_CHUNK_SECTORS ? IDE_CHUNK_SECTORS : count;
        e = ata_pio(ATA_WRITE, drive, lba, n, (void *)p);
        if (e) return e;
        lba += n; count -= n; p += n * IDE_SECTOR_SIZE;
    }
    return IDE_OK;
}

int ide_flush_drive(int drive)
{
    int e = check_drive(drive, 0, 0);
    if (e) return e;

    int ch = ide_devices[drive].channel;
    e = select_drive(ch, ide_devices[drive].Drive, 0);
    if (e) return e;

    reg_out(ch, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ide_poll(ch, 0);
}

void ide_read_sectors(unsigned char drive, unsigned char numsects,
                      unsigned int lba, unsigned char *buffer)
{
    package[0] = (unsigned char)ide_read_blocks(drive, lba, numsects, buffer);
}

void ide_write_sectors(unsigned char drive, unsigned char numsects,
                       unsigned int lba, unsigned char *buffer)
{
    int e = ide_write_blocks(drive, lba, numsects, buffer);
    if (e == IDE_OK)
        e = ide_flush_drive(drive);
    package[0] = (unsigned char)e;
}

/* ---------- blockdev ---------- */

static int bd_index(void *ctx)
{
    return (int)((struct ide_devices_s *)ctx - ide_devices);
}

static int bd_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    return ide_read_blocks(bd_index(ctx), lba, count, buf) == IDE_OK ? 0 : -1;
}

static int bd_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    return ide_write_blocks(bd_index(ctx), lba, count, buf) == IDE_OK ? 0 : -1;
}

static int bd_flush(void *ctx)
{
    return ide_flush_drive(bd_index(ctx)) == IDE_OK ? 0 : -1;
}

blockdev_t *ide_blockdev(int drive)
{
    if (check_drive(drive, 0, 0) != IDE_OK)
        return 0;

    blockdev_t *bd = &ide_bd[drive];
    bd->ctx         = &ide_devices[drive];
    bd->block_size  = IDE_SECTOR_SIZE;
    bd->block_count = ide_devices[drive].Size;
    bd->read        = bd_read;
    bd->write       = bd_write;
    bd->flush       = bd_flush;
    bd->read_only   = 0;
    return bd;
}
