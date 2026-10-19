#ifndef IDE_IO_H
#define IDE_IO_H

#include <stdint.h>
#include "blockdev.h"

/*
 * Legacy IDE/ATA (PIO, LBA28) — те, що емулюють QEMU/VirtualBox/VMware (-drive if=ide)
 * і що дає режим сумісності в BIOS на залізі. Порти фіксовані: primary 0x1F0/0x3F6,
 * secondary 0x170/0x376. DMA, переривання, LBA48 і ATAPI поки нема: диск видно в межах
 * перших 128 ГіБ, читання/запис — PIO по секторах.
 *
 * Номер диска (drive, "hdN") — порядок знаходження: 0..3 серед усіх знайдених пристроїв.
 */

#define IDE_MAX_DRIVES   4
#define IDE_SECTOR_SIZE  512

/* коди помилок (package[0], ide_strerror) */
#define IDE_OK           0
#define IDE_E_DF         1   /* Device Fault */
#define IDE_E_ERR        2   /* пристрій виставив ERR */
#define IDE_E_NODRIVE    3   /* такого диска нема */
#define IDE_E_TIMEOUT    4   /* BSY/DRQ не дочекались */
#define IDE_E_RANGE      5   /* LBA за межами диска (або вище LBA28) */
#define IDE_E_NOLBA      6   /* диск без LBA */
#define IDE_E_NOTATA     7   /* це ATAPI (CD) — читання поки не вміємо */

#define IDE_ATA          0x00   /* ide_devices[].Type (значення збігаються з тими, що давно є в shell.c) */
#define IDE_ATAPI        0x01

struct IDEChannelRegisters
{
    unsigned int  base;     /* командний блок: 0x1F0 / 0x170 */
    unsigned int  ctrl;     /* порт Device Control / Alt Status: 0x3F6 / 0x376 */
    unsigned int  bmide;
    unsigned char nIEN;
};

struct ide_devices_s
{
    unsigned char  Reserved;     /* 1 — слот зайнятий */
    unsigned char  channel;
    unsigned char  Drive;        /* 0 master, 1 slave */

    unsigned short Type;         /* 0 ATA, 1 ATAPI */
    unsigned short Signature;
    unsigned short Capabilities;

    unsigned int   CommandSets;
    unsigned int   Size;         /* секторів по 512 Б */

    unsigned char  Model[41];
};

extern struct IDEChannelRegisters channels[2];
extern unsigned char ide_buf[2048];
extern unsigned char atapi_packet[12];
extern struct ide_devices_s ide_devices[IDE_MAX_DRIVES];
extern unsigned char package[2];   /* package[0] — код останньої помилки ide_read/write_sectors */

/* BAR0..BAR4 = 0 — класичні порти (так і викликає kernel.c) */
void ide_init(unsigned int BAR0, unsigned int BAR1,
              unsigned int BAR2, unsigned int BAR3,
              unsigned int BAR4);

int  ide_drive_count(void);                       /* скільки пристроїв знайдено (ATA + ATAPI) */
const char *ide_strerror(int code);

/* сирий доступ; повертає IDE_OK або IDE_E_*. count — будь-яке (ріжеться на шматки по 128 секторів).
 * ide_write_blocks не скидає кеш диска — це робить ide_flush_drive (blockdev.flush викликає саме його). */
int  ide_read_blocks(int drive, uint32_t lba, uint32_t count, void *buf);
int  ide_write_blocks(int drive, uint32_t lba, uint32_t count, const void *buf);
int  ide_flush_drive(int drive);

/* стара пара функцій (результат у package[0]); write тут одразу робить CACHE FLUSH */
void ide_read_sectors(unsigned char drive, unsigned char numsects,
                      unsigned int lba, unsigned char *buffer);
void ide_write_sectors(unsigned char drive, unsigned char numsects,
                       unsigned int lba, unsigned char *buffer);

/* blockdev для fat_mount() та ін.; NULL — диска нема або це не ATA. Масив статичний. */
blockdev_t *ide_blockdev(int drive);

#endif /* IDE_IO_H */
