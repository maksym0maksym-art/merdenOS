#include "../../headers/AHCI.h"
#include <stdint.h>
#include "../../../standarts_lib/stdio.h"

HBA_PORT *g_port = NULL;

#define NULL ((void*)0)

void* memset(void* dest, int value, unsigned int count) 
{
    unsigned char* ptr = (unsigned char*)dest;

    while(count--)
    {
        *ptr++ = (unsigned char)value;
    }

    return dest;
}

void AHCI_INIT(HBA_MEM *abar)
{

    if (!abar) return;

    // 1. Вмикаємо AHCI-режим у контролері (GHC.AE = 1)
    abar->ghc |= (1 << 31);

    // 2. Запускаємо пошук і перевірку портів
    probe_port(abar);
}

void reset_port(HBA_PORT *port) 
{
    stop_cmd(port);

    port->sact = 0;
    port->serr = (uint32_t)-1; 
    
    for (volatile int i = 0; i < 1000000; i++);

    start_cmd(port);
}

#define SATA_SIG_ATA    0x00000101 
#define SATA_SIG_SATAPI  0xEB140101  
#define SATA_SIG_SEMB   0xC33C0101
#define SATA_SIG_PM     0x96690101

#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_SEMB 2
#define AHCI_DEV_PM 3
#define AHCI_DEV_SATAPI 4

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

static int check_type(HBA_PORT *port)
{
    uint32_t ssts = port->ssts;

    uint8_t ipm =(ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F; 

    t_printf("DET = ");
    t_print_int(det);
    t_putchar('\n');

    t_printf("IPM = ");
    t_print_int(ipm);
    t_putchar('\n');

    if (det != HBA_PORT_DET_PRESENT)
    {
        return AHCI_DEV_NULL;
    }
    if (ipm != HBA_PORT_IPM_ACTIVE)
    {
        return AHCI_DEV_NULL;
    }

    switch(port->sig)
    {
    case SATA_SIG_SATAPI:
        return AHCI_DEV_SATAPI;
    
    case SATA_SIG_PM:
        return AHCI_DEV_PM; 
    case SATA_SIG_ATA:
        return AHCI_DEV_SATA;
    default:
        return AHCI_DEV_NULL;
    }
}

#define HBA_PxIS_TFES (1 << 30)

int ahci_read(HBA_PORT *port, uint64_t lba, uint32_t sector_count, void *buffer)
{
    int slot = find_cmdslot(port);
    if (slot == -1)
    {
        return -1;
    }

    port->is = (uint32_t)-1;
    wait_busy(port);

    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)(uintptr_t)port->clb;
    
    cmdheader += slot;

    cmdheader->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    
    cmdheader->w = 0;
    cmdheader->prdtl = 1;

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL *)(uintptr_t)cmdheader->ctba;

    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL) + (cmdheader->prdtl - 1) * sizeof(HBA_PRDT_ENTRY));

    cmdtbl->prdt_entry[0].dba = (uint32_t)(uintptr_t)buffer;
    cmdtbl->prdt_entry[0].dbau = 0;

    cmdtbl->prdt_entry[0].dbc = (sector_count * 512) - 1;

    cmdtbl->prdt_entry[0].i = 1;

    FIS_REG_H2D *fis = (FIS_REG_H2D *)cmdtbl->cfis;
    memset(fis, 0, sizeof(FIS_REG_H2D));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;

    fis->command = 0x25;

    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->device = 1 << 6;

    fis->countl = sector_count & 0xFF;
    fis->counth = sector_count >> 8;

    port->ci = 1 << slot;

    while  (port->ci & (1 << slot))
    {
        if (port->is & HBA_PxIS_TFES)
    {
        return -1;
    }
    }

    if (port->is &(1 << 30))
    {
        return -1;
    }

    if (port->is & HBA_PxIS_TFES)
    {
        return -1;
    }

    if (sector_count == 0 || sector_count > 65535)
    {
        return -1;
    }

    return 0;

}



#define ATA_CMD_WRITE_DMA_EXT 0x35

int ahci_write(HBA_PORT *port, uint64_t lba, uint32_t sector_count, const void *buffer)
{
    if (sector_count == 0 || sector_count > 65535)
        return -1;

    int slot = find_cmdslot(port);
    if (slot == -1)
        return -1;

    port->is = (uint32_t)-1;

    wait_busy(port);

    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)(uintptr_t)port->clb;
    cmdheader += slot;

    cmdheader->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);

    cmdheader->w = 1;              // <-- запис
    cmdheader->prdtl = 1;

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL *)(uintptr_t)cmdheader->ctba;

    memset(cmdtbl, 0,
           sizeof(HBA_CMD_TBL) +
           (cmdheader->prdtl - 1) * sizeof(HBA_PRDT_ENTRY));

    cmdtbl->prdt_entry[0].dba = (uint32_t)(uintptr_t)buffer;
    cmdtbl->prdt_entry[0].dbau = 0;
    cmdtbl->prdt_entry[0].dbc = sector_count * 512 - 1;
    cmdtbl->prdt_entry[0].i = 1;

    FIS_REG_H2D *fis = (FIS_REG_H2D *)cmdtbl->cfis;

    memset(fis, 0, sizeof(FIS_REG_H2D));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;

    fis->command = ATA_CMD_WRITE_DMA_EXT;

    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->device = 1 << 6;

    fis->countl = sector_count & 0xFF;
    fis->counth = (sector_count >> 8) & 0xFF;

    port->ci = 1 << slot;

    while (port->ci & (1 << slot))
    {
        if (port->is & HBA_PxIS_TFES)
            return -1;
    }

    if (port->is & HBA_PxIS_TFES)
        return -1;

    return 0;
}


/*   void probe_port(HBA_MEM *abar)
{
    uint32_t pi = abar->pi;
    int i = 0;
    while(i < 32)
    {
        if (pi & 1)
        {
            int dt = check_type(&abar->ports[i]);
            if (dt == AHCI_DEV_SATA)
            {
                t_printf("SATA drive found at port %d\n", i);

                g_port = &abar->ports[i];
                port_rebase(g_port, i);
            }
            else if (dt == AHCI_DEV_SATAPI)
            {
                t_printf("SATAPI drive found at port %d\n", i);
                t_printf("PI = %x\n", abar->pi);
            }
            else if(dt == AHCI_DEV_PM)
            {
                t_printf("PM drive found at port %d\n", i);
            }
            else if(dt == AHCI_DEV_SEMB)
            {
                t_printf("SEMB port found at port %d\n", i);
            }
            else 
            {
                t_printf("No drive founded at ports %d\n", i);
                t_printf("PI = %x\n", abar->pi);
            }
        }

        t_printf("PI = %x\n", abar->pi);

        pi >>= 1;
        i++;

    }
}
*/

void probe_port(HBA_MEM *abar)
{
    uint32_t pi = abar->pi;
    t_printf("AHCI PI (Ports Implemented) = 0x%x\n", pi);
/*
    for (int i = 0; i < 32; i++)
    {
        // Перевіряємо, чи існує порт під номером i
        if (pi & (1 << i))
        {
            HBA_PORT *port = &abar->ports[i];
            
            // Виводимо стан порту для відладки
            t_printf("Port %d: SSTS = 0x%x, SIG = 0x%x\n", i, port->ssts, port->sig);

            int dt = check_type(port);
            if (dt == AHCI_DEV_SATA)
            {
                t_printf("SATA drive found at port %d\n", i);
                g_port = port;
                port_rebase(g_port, i);
                return; // Знайшли диск, зберігаємо його в g_port та виходимо
            }
            else if (dt == AHCI_DEV_SATAPI)
            {
                t_printf("SATAPI drive found at port %d\n", i);
            }
        }
    }
*/

    for (int i = 0; i < 6; i++)
    {
        HBA_PORT *port = &abar->ports[i];

        t_printf("ABAR = ");
        t_printf_hex((uint32_t)abar);
        t_putchar('\n');

        t_printf("sizeof(HBA_PORT) = ");
        t_print_int(sizeof(HBA_PORT));
        t_putchar('\n');

        t_printf("PORT0 = ");
        t_printf_hex((uint32_t)port);
        t_putchar('\n');

        t_printf("OFFSET = ");
        t_printf_hex((uint32_t)port - (uint32_t)abar);
        t_putchar('\n');

        t_printf("\nPORT ");
        t_print_int(i);
        t_putchar('\n');

        t_printf("SSTS = ");
        t_printf_hex(port->ssts);
        t_putchar('\n');

        t_printf("SIG  = ");
        t_printf_hex(port->sig);
        t_putchar('\n');

        t_printf("CMD  = ");
        t_printf_hex(port->cmd);
        t_putchar('\n');

        t_printf("TFD  = ");
        t_printf_hex(port->tfd);
        t_putchar('\n');

        int type = check_type(port);

        t_printf("TYPE = ");
        t_print_int(type);
        t_putchar('\n');
        check_type(port);

        if (type == AHCI_DEV_SATA)
        {
            t_printf("SATA drive found at port ");
            t_print_int(i);
            t_putchar('\n');

            g_port = port;
            port_rebase(g_port, i);

            return;
}

    }

    t_printf("No SATA drives detected in any port.\n");
}

#define AHCI_BASE   0x400000

#define HBA_RxCMD_ST  0x0001
#define HBA_RxCMD_FREE 0x0010
#define HBA_RxCMDFR 0x4000
#define HBA_RxCMD_CR 0x8000

void start_cmd(HBA_PORT *port)
{
    while (port->cmd & HBA_RxCMD_CR);

    port->cmd  |= HBA_RxCMD_FREE;
    port->cmd  |= HBA_RxCMD_ST;
}

void stop_cmd(HBA_PORT *port)
{
    port->cmd &= ~HBA_RxCMD_ST;

    port->cmd &= ~HBA_RxCMD_FREE;

    while(1)
    {
        if(port->cmd & HBA_RxCMDFR)
        {
            continue;
        }
        if(port->cmd & HBA_RxCMD_CR)
        {
            continue;
        }
        break;

    }
}

void port_rebase(HBA_PORT *port, int portno)
{ 
    stop_cmd(port);

    port->clb = AHCI_BASE + (portno<<10);
    port->clbu = 0; 
    memset((void*)(port->clb), 0, 1024); 
    
    port->fb = AHCI_BASE + (32<<10) + (portno<<8);
    port->fbu = 0; 
    memset((void*)(port->fb), 0, 256);


    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER*)(port->clb);
    for(int i = 0; i < 32; i++) 
    {
        cmdheader[i].prdtl = 8;
                            
        cmdheader[i].ctba = AHCI_BASE + (40<<10) + (portno<<13) + (i<<8);
        cmdheader[i].ctbau = 0; 
        memset((void*)cmdheader[i].ctba, 0, 256);
    }
    start_cmd(port);
}

#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_INDETIFY        0xEC

int find_cmdslot(HBA_PORT *port)
{
    uint32_t slots = port->sact | port->ci;

    for (int i = 0; i < 32; i++)
    {
        if ((slots &(1 << i)) == 0)
        {
            return i;
        }
    }
    
    return -1;

}

#define ATA_DEV_BUSY    0x80
#define ATA_DEV_DRQ     0x08

int wait_busy(HBA_PORT *port)
{
    while (port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ));

    return 0;
}


#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) |
                       (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

HBA_MEM* ahci_get_base_address(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                
                uint32_t class_reg = pci_read(bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;

                if (class_code == 0x01 && subclass == 0x06) {
                    t_printf("AHCI controller found!\n");

                    uint32_t bar5 = pci_read(bus, slot, func, 0x24);

                    t_printf("BAR5 = ");
                    t_printf_hex(bar5);
                    t_putchar('\n');
                    
                    return (HBA_MEM*)(uintptr_t)(bar5 & 0xFFFFFFF0);
                }
            }
        }
    }
    return NULL;
}
