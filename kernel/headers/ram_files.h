#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

struct bootsector;

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

FS_FILE *fs_find(const char *name);
FS_FILE *fs_get_or_create();

/* для провідника */
int fs_new_file(const char *name);
int fs_delete(const char *name);
int fs_rename_file(const char *from, const char *to);

#endif