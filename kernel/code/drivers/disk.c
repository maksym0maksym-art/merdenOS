#include "../../headers/disk.h"
#include "../../headers/AHCI.h"

int disk_read(HBA_PORT *port ,uint64_t lba, uint32_t sectors, void *buffer)
{
    return ahci_read(port, lba, sectors, buffer);
}

int disk_write(HBA_PORT *port, uint64_t lba, uint32_t sectors, const void *buffer)
{
    return ahci_write(port, lba, sectors, buffer);
}