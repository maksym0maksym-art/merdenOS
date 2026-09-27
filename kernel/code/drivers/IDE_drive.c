#include "io.h"
#include "../../headers/system.h"
//#include "../../standarts_lib/stdio.h"

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DSC   0x10
#define ATA_SR_DRQ   0x08
#define ATA_SR_CORR  0x04
#define ATA_SR_IDX   0x02
#define ATA_SR_ERR   0x01

#define ATA_ER_BBK   0x80
#define ATA_ER_UNC   0x40
#define ATA_ER_MC    0x20
#define ATA_ER_IDNF  0x10
#define ATA_ER_MCR   0x08
#define ATA_ER_ABRT  0x04
#define ATA_ER_TKONF 0x02
#define ATA_ER_AMNF  0x01

#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_IDENTIFY        0xEC

#define ATA_IDENT_DEVICETYPE   0
#define ATA_IDENT_CAPABILITIES 98
#define ATA_IDENT_MODEL        54
#define ATA_IDENT_COMMANDSETS  164
#define ATA_IDENT_MAX_LBA      120
#define ATA_IDENT_MAX_LBA_EXT  200

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_CONTROL    0x0C
#define ATA_REG_ALTSTATUS  0x0C

#define ATA_PRIMARY    0x00
#define ATA_SECONDARY  0x01
#define ATA_READ       0x00
#define ATA_WRITE      0x01
#define IDE_ATA        0x00
#define IDE_ATAPI      0x01

#define IDE_TIMEOUT_ITERS   100000

struct IDEChannelRegisters
{
    unsigned int  base;
    unsigned int  ctrl;
    unsigned int  bmide;
    unsigned char nIEN;
} channels[2];

unsigned char ide_buf[2048] = {0};
unsigned char atapi_packet[12] = {0xA8, 0,0,0,0,0,0,0,0,0,0,0};

struct ide_device_s
{
    unsigned char  Reserved;   
    unsigned char  channel;
    unsigned char  Drive;
    unsigned short Type;
    unsigned short Signature;
    unsigned short Capabilities;
    unsigned int   CommandSets;
    unsigned int   Size;
    unsigned char  Model[41];
} ide_devices[4];

unsigned char package[2] = {0, 0};

static inline void io_delay(void)
{
        inb(0x80);
}

void ide_write(unsigned char channel, unsigned char reg, unsigned char data)
{
    if (reg > 0x07 && reg < 0x0C)
        ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);

    if (reg < 0x08)
        outb(channels[channel].base + reg, data);
    else if (reg < 0x0C)
        outb(channels[channel].base + reg - 0x06, data);
    else if (reg < 0x0E)
        outb(channels[channel].ctrl + reg - 0x0A, data);
    else if (reg < 0x16)
        outb(channels[channel].bmide + reg - 0x0E, data);

    if (reg > 0x07 && reg < 0x0C)
        ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

unsigned char ide_read(unsigned char channel, unsigned char reg)
{
    unsigned char result = 0;

    if (reg > 0x07 && reg < 0x0C)
        ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);

    if (reg < 0x08)
        result = inb(channels[channel].base + reg);
    else if (reg < 0x0C)
        result = inb(channels[channel].base + reg - 0x06);
    else if (reg < 0x0E)
        result = inb(channels[channel].ctrl + reg - 0x0A);
    else if (reg < 0x16)
        result = inb(channels[channel].bmide + reg - 0x0E);

    if (reg > 0x07 && reg < 0x0C)
        ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);

    return result;
}

static inline void insw(unsigned short port, void *addr, unsigned int count)
{
    asm volatile("cld\n\trep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(unsigned short port, void *addr, unsigned int count)
{
    asm volatile("cld\n\trep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

void ide_read_buffer(unsigned char channel, unsigned char reg, void *buffer, unsigned int words)
{
    if (reg < 0x08)
        insw(channels[channel].base + reg, buffer, words);
    else if (reg < 0x0C)
        insw(channels[channel].base + reg - 0x06, buffer, words);
}


unsigned char ide_poll_status(unsigned char channel, unsigned int check_drq)
{
    
    for (int i = 0; i < 4; i++)
        ide_read(channel, ATA_REG_ALTSTATUS);

    unsigned int timeout = 0;
    unsigned char status;

    while ((status = ide_read(channel, ATA_REG_STATUS)) & ATA_SR_BSY)
    {
        if (++timeout > IDE_TIMEOUT_ITERS)
            return 4;
    }

    if (status & ATA_SR_ERR)
        return 2;

    if (status & ATA_SR_DF)
        return 1;

    if (check_drq)
    {
        timeout = 0;
        while (!((status = ide_read(channel, ATA_REG_STATUS)) & ATA_SR_DRQ))
        {
            if (status & ATA_SR_ERR)
                return 2;

            if (++timeout > IDE_TIMEOUT_ITERS)
                return 4;
        }
    }

    return 0;
}

static int ide_identify_device(unsigned char channel, unsigned char drive, int *out_type)
{
    ide_write(channel, ATA_REG_HDDEVSEL, 0xA0 | (drive << 4));
    io_delay();

    unsigned char status = ide_read(channel, ATA_REG_STATUS);

    if (status == 0xFF)
        return 0;

    ide_write(channel, ATA_REG_SECCOUNT0, 0);
    ide_write(channel, ATA_REG_LBA0, 0);
    ide_write(channel, ATA_REG_LBA1, 0);
    ide_write(channel, ATA_REG_LBA2, 0);
    ide_write(channel, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_delay();

    status = ide_read(channel, ATA_REG_STATUS);
    if (status == 0)
        return 0; 

    unsigned int timeout = 0;
    int type = IDE_ATA;

    while (1)
    {
        status = ide_read(channel, ATA_REG_STATUS);

        if (status & ATA_SR_ERR)
        {
            unsigned char cl = ide_read(channel, ATA_REG_LBA1);
            unsigned char ch = ide_read(channel, ATA_REG_LBA2);

            if ((cl == 0x14 && ch == 0xEB) || (cl == 0x69 && ch == 0x96))
            {
                type = IDE_ATAPI;
                ide_write(channel, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
                io_delay();
                timeout = 0;
                
                while (!((status = ide_read(channel, ATA_REG_STATUS)) & ATA_SR_DRQ))
                {
                    if (status & ATA_SR_ERR) return 0;
                    if (++timeout > IDE_TIMEOUT_ITERS) return 0;
                }
                break;
            }
            return 0;
        }

        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            break; 
        if (++timeout > IDE_TIMEOUT_ITERS)
            return 0; 
    }

    ide_read_buffer(channel, ATA_REG_DATA, ide_buf, 256);
    *out_type = type;
    return 1;
}

void ide_init(unsigned int BAR0, unsigned int BAR1,
              unsigned int BAR2, unsigned int BAR3,
              unsigned int BAR4)
{
    int count = 0;

    channels[ATA_PRIMARY].base    = (BAR0 & 0xFFFFFFFC) + 0x1F0 * (!BAR0);
    channels[ATA_PRIMARY].ctrl    = (BAR1 & 0xFFFFFFFC) + 0x3F6 * (!BAR1);
    channels[ATA_SECONDARY].base  = (BAR2 & 0xFFFFFFFC) + 0x170 * (!BAR2);
    channels[ATA_SECONDARY].ctrl  = (BAR3 & 0xFFFFFFFC) + 0x376 * (!BAR3);
    channels[ATA_PRIMARY].bmide   = (BAR4 & 0xFFFFFFFC);
    channels[ATA_SECONDARY].bmide = (BAR4 & 0xFFFFFFFC) + 8;

    channels[ATA_PRIMARY].nIEN   = 2;
    channels[ATA_SECONDARY].nIEN = 2;
    ide_write(ATA_PRIMARY,   ATA_REG_CONTROL, 2);
    ide_write(ATA_SECONDARY, ATA_REG_CONTROL, 2);

    for (int i = 0; i < 4; i++)
        ide_devices[i].Reserved = 0;

    for (unsigned char ch = 0; ch < 2; ch++)
    {
        for (unsigned char drv = 0; drv < 2; drv++)
        {
            int type = IDE_ATA;

            if (!ide_identify_device(ch, drv, &type))
                continue; /* тут раніше могло зависнути назавжди */

            ide_devices[count].Reserved  = 1;
            ide_devices[count].Type      = type;
            ide_devices[count].channel   = ch;
            ide_devices[count].Drive     = drv;
            ide_devices[count].Signature =
                *((unsigned short *)(ide_buf + ATA_IDENT_DEVICETYPE));
            ide_devices[count].Capabilities =
                *((unsigned short *)(ide_buf + ATA_IDENT_CAPABILITIES));
            ide_devices[count].CommandSets =
                *((unsigned int *)(ide_buf + ATA_IDENT_COMMANDSETS));

            if (ide_devices[count].CommandSets & (1 << 26))
                ide_devices[count].Size =
                    *((unsigned int *)(ide_buf + ATA_IDENT_MAX_LBA_EXT));
            else
                ide_devices[count].Size =
                    *((unsigned int *)(ide_buf + ATA_IDENT_MAX_LBA));

            for (int k = 0; k < 40; k += 2)
            {
                ide_devices[count].Model[k]     = ide_buf[ATA_IDENT_MODEL + k + 1];
                ide_devices[count].Model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
            }
            ide_devices[count].Model[40] = 0;

            for (int k = 39; k >= 0 && ide_devices[count].Model[k] == ' '; k--)
                ide_devices[count].Model[k] = 0;

            count++;
        }
    }
}

unsigned char ide_ata_access(unsigned char direction, unsigned char drive,
                              unsigned int lba, unsigned char numsects,
                              unsigned char *buffer)
{
    unsigned char channel  = ide_devices[drive].channel;
    unsigned char slavebit = ide_devices[drive].Drive;
    unsigned int  bus      = channels[channel].base;
    unsigned char head     = (unsigned char)((lba >> 24) & 0x0F);

    if (lba >= 0x10000000)
        return 5; 

    if (!(ide_devices[drive].Capabilities & 0x200))
        return 6;

    unsigned int timeout = 0;
    while (ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY)
    {
        if (++timeout > IDE_TIMEOUT_ITERS)
            return 4;
    }

    ide_write(channel, ATA_REG_HDDEVSEL, 0xE0 | (slavebit << 4) | head);
    ide_write(channel, ATA_REG_SECCOUNT0, numsects);
    ide_write(channel, ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
    ide_write(channel, ATA_REG_LBA1, (unsigned char)((lba >> 8)  & 0xFF));
    ide_write(channel, ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));

    ide_write(channel, ATA_REG_COMMAND,
              (direction == ATA_READ) ? ATA_CMD_READ_PIO : ATA_CMD_WRITE_PIO);

    if (direction == ATA_READ)
    {
        for (int i = 0; i < numsects; i++)
        {
            unsigned char err = ide_poll_status(channel, 1);
            if (err) return err;
            insw(bus + ATA_REG_DATA, buffer + i * 512, 256);
        }
    }
    else
    {
        for (int i = 0; i < numsects; i++)
        {
            unsigned char err = ide_poll_status(channel, 1);
            if (err) return err;
            outsw(bus + ATA_REG_DATA, buffer + i * 512, 256);
        }
        ide_write(channel, ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        ide_poll_status(channel, 0);
    }

    return 0;
}

void ide_read_sectors(unsigned char drive, unsigned char numsects,
                       unsigned int lba, unsigned char *buffer)
{
    if (drive > 3 || ide_devices[drive].Reserved == 0)
    {
        package[0] = 1;
        return;
    }

    if (ide_devices[drive].Type != IDE_ATA)
    {
        package[0] = 7; 
        return;
    }

    if ((lba + numsects) > ide_devices[drive].Size)
    {
        package[0] = 2; 
        return;
    }

    package[0] = ide_ata_access(ATA_READ, drive, lba, numsects, buffer);
}

void ide_write_sectors(unsigned char drive, unsigned char numsects,
                        unsigned int lba, unsigned char *buffer)
{
    if (drive > 3 || ide_devices[drive].Reserved == 0)
    {
        package[0] = 1;
        return;
    }

    if (ide_devices[drive].Type != IDE_ATA)
    {
        package[0] = 4;
        return;
    }

    if ((lba + numsects) > ide_devices[drive].Size)
    {
        package[0] = 2;
        return;
    }

    package[0] = ide_ata_access(ATA_WRITE, drive, lba, numsects, buffer);
}