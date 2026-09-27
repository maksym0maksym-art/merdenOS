#ifndef EXFAT_H
#define EXFAT_H

#include <stdint.h>
#include "AHCI.h"

/*struct bootsector;

uint32_t BootChecksum(const uint8_t * Sectors, uint16_t BytesPerSector);

const char *fs_read(const char *name );

void prints_file_info(char *name);

int fs_get_file_count(void);

typedef struct{
	char name[64];
    char data[1024];
	unsigned int size;
} FS_FILE;

FS_FILE *fs_get_files(void);

char *fs_create(const char *name);

//FS_FILE *list = fs_get_files;


int exfat_mount(HBA_PORT *port);

int exfat_read_cluster(HBA_PORT *port,
                       uint32_t cluster,
                       void *buffer);


uint32_t exfat_next_cluster(HBA_PORT *port, uint32_t cluster);

uint32_t exfat_allocate_cluster(HBA_PORT *port);

int exfat_create_file(HBA_PORT *port, const char *name);
*/
#endif