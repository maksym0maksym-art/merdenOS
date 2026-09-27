#ifndef IDE_IO_H
#define IDE_IO_H

struct IDEChannelRegisters
{
    unsigned int  base;
    unsigned int  ctrl;
    unsigned int  bmide;
    unsigned char nIEN;
};

/* ============================================================
 * IDE DEVICE INFORMATION
 * ============================================================ */

struct ide_devices_s
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
};

/* ============================================================
 * GLOBAL DRIVER STATE
 * ============================================================ */

extern struct IDEChannelRegisters channels[2];

extern unsigned char ide_buf[2048];

extern volatile unsigned char ide_irq_invoked;

extern unsigned char atapi_packet[12];

extern struct ide_devices_s ide_devices[4];

extern unsigned char package[2];

/* ============================================================
 * LOW-LEVEL I/O
 * ============================================================ */

static inline void io_delay(void);

static inline void insl(
    unsigned short port,
    void *addr,
    unsigned int count
);

static inline void insw(
    unsigned short port,
    void *addr,
    unsigned int count
);

static inline void outsw(
    unsigned short port,
    void *addr,
    unsigned int count
);

/* ============================================================
 * IDE REGISTER ACCESS
 * ============================================================ */

void ide_write(
    unsigned char channel,
    unsigned char reg,
    unsigned char data
);

unsigned char ide_read(
    unsigned char channel,
    unsigned char reg
);

void ide_read_buffer(
    unsigned char channel,
    unsigned char reg,
    void *buffer,
    unsigned int quads
);

/* ============================================================
 * IDE STATUS / ERROR
 * ============================================================ */

unsigned char ide_pooling(
    unsigned char channel,
    unsigned int advanced_check
);

unsigned char ide_print_error(
    unsigned int drive,
    unsigned char err
);

/* ============================================================
 * IDE INITIALIZATION
 * ============================================================ */

void ide_init(
    unsigned int BAR0,
    unsigned int BAR1,
    unsigned int BAR2,
    unsigned int BAR3,
    unsigned int BAR4
);

/* ============================================================
 * ATA / ATAPI ACCESS
 * ============================================================ */

unsigned char ide_ata_access(
    unsigned char direction,
    unsigned char drive,
    unsigned int lba,
    unsigned char numsects
);

unsigned char ide_atapi_read(
    unsigned char drive,
    unsigned int lba,
    unsigned char numsects,
    unsigned short es,
    unsigned int edi
);

/* ============================================================
 * SECTOR OPERATIONS
 * ============================================================ */

void ide_read_sectors(
    unsigned char drive,
    unsigned char numsects,
    unsigned int lba,
    unsigned char *buffer
);

void ide_write_sectors(
    unsigned char drive,
    unsigned char numsects,
    unsigned int lba,
    unsigned short es,
    unsigned int edi
);

#endif /* IDE_IO_H */