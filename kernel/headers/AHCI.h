#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include "AHCI.h"

#define NULL ((void*)0)

typedef enum
{

    FIS_TYPE_REG_H2D        = 0x27,
    FIS_TYPE_REG_D2H        = 0x34,
    FIS_TYPE_DMA_ACT        = 0x39,
    FIS_TYPE_DMA_SETUP      = 0x41,
    FIS_TYPE_DATA           = 0x46,
    FIS_TYPE_BIST           = 0x58,
    FIS_TYPE_PIO_SETUP      = 0x5F,
    FIS_TYPE_DEV_BITS       = 0xA1,

} FIS_TYPE;

typedef struct __attribute__((packed)) tagFIS_REG_H2D
{
    uint8_t fis_type;

    uint8_t pmport:4;
    uint8_t rvs0:3;
    uint8_t c:1;

    uint8_t command;
    uint8_t featurel;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;

    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;

    uint8_t rsv1[4];

}   FIS_REG_H2D;

typedef struct __attribute__((packed)) tagFIS_REG_D2H
{

    uint8_t fis_type;

    uint8_t pmport:4;
    uint8_t rsv0:2;
    uint8_t i:1;
    uint8_t rsv1:1;

    uint8_t status;
    uint8_t error;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t rsv2;

    uint8_t countl;
    uint8_t counth;
    uint8_t rsv3[2];

    uint8_t rsv4[4];

}   FIS_REG_D2H;

typedef struct  __attribute__((packed)) tagFIS_DATA
{

    uint8_t fis_type;

    uint8_t pmport:4;
    uint8_t rsv0:4;

    uint8_t rsv1[2];

    uint32_t data;

}   FIS_DATA;

typedef struct __attribute__((packed)) tagFIS_PIO_SETUP
{

    uint8_t fis_type;

    uint8_t pmport:4;
    uint8_t rsv0:1;
    uint8_t d:1;
    uint8_t i:1;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t rsv2;

    uint8_t countl;
    uint8_t counth;
    uint8_t rsv3[2];
    uint8_t e_status;

    uint16_t tc;
    uint8_t rsv4[4];

}   FIS_PIO_SETUP;


typedef struct  __attribute__((packed)) tagFIS_DMA_SETUP
{
    uint8_t  fis_type;

	uint8_t  pmport:4;
	uint8_t  rsv0:1;
	uint8_t  d:1;
	uint8_t  i:1;
	uint8_t  a:1;

    uint8_t  rsved[2];

	//DWORD 1&2

    uint64_t DMAbufferID;


    //DWORD 3
    uint32_t rsvd;
    uint32_t DMAbufOffset;
    uint32_t TransferCount;
    uint32_t resvd;
}   FIS_DMA_SETUP;

void* memset(void* dest, int value, unsigned int count);

typedef volatile struct tagHBA_PORT
{
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} HBA_PORT;

extern HBA_PORT *g_port;

typedef volatile struct __attribute__((packed)) tagHBA_MEM
{

    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;

    uint8_t rsv[0xA0-0x2c];

    uint8_t vendor[0x100-0xA0];

    HBA_PORT    ports[1];

}   HBA_MEM;

typedef struct __attribute__((packed))
{
    uint8_t fis_type;      // 0xA1

    uint8_t pmport:4;
    uint8_t rsv0:2;
    uint8_t i:1;
    uint8_t rsv1:1;

    uint8_t status_lo;
    uint8_t status_hi;

    uint32_t error;

    uint32_t protocol;

    uint8_t rsv2[4];

} FIS_DEV_BITS;

typedef volatile struct tagHBA_FIS
{

    FIS_DMA_SETUP   dsfis;
    uint8_t         pad0[4];


    FIS_PIO_SETUP   psfis;
    uint8_t         pad1[12];


    FIS_REG_D2H rfis;
    uint8_t         pad2[4];


    FIS_DEV_BITS    sdbfis;


    uint8_t         ufis[64];

    uint8_t     rsv[0x100-0xA0];
} HBA_FIS;

typedef struct tagHBA_CMD_HEADER
{

    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;

    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;

    uint16_t prdtl;


    volatile
    uint32_t prdbc;


    uint32_t ctba;
    uint32_t ctbau;


    uint32_t rsv1[4];
} HBA_CMD_HEADER;


typedef struct tagHBA_PRDT_ENTRY
{

    uint32_t    dba;
    uint32_t    dbau;
    uint32_t    rsv0;

    uint32_t    dbc:22;
    uint32_t    rsv1:9;
    uint32_t    i:1;

}   HBA_PRDT_ENTRY;

typedef struct __attribute__((packed))
{
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];

    HBA_PRDT_ENTRY prdt_entry[1];

} HBA_CMD_TBL;

void reset_port(HBA_PORT *port) ;

void* memset(void* dest, int value, unsigned int count);


void AHCI_INIT(HBA_MEM *abar);

void probe_port(HBA_MEM *abar);

void start_cmd(HBA_PORT *port);

void stop_cmd(HBA_PORT *port);

void port_rebase(HBA_PORT *port, int portno);

static int check_type(HBA_PORT *port);


int ahci_read(HBA_PORT *port, uint64_t lba, uint32_t sector_count, void *buffer);

int ahci_write(HBA_PORT *port, uint64_t lba, uint32_t sector_count, const void *buffer);


int find_cmdslot(HBA_PORT *port);

int wait_busy(HBA_PORT *port);

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

HBA_MEM* ahci_get_base_address(void);



#endif
