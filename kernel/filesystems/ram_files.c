#include "../headers/ram_files.h"
#include "../../standarts_lib/stdio.h"
#include "../headers/kernel-panic.h"
#include "../headers/vga.h"
#include "../../system/shell/shell.h"
#define STR_SIZE(str) (sizeof(str) - 1)
#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

#define MAX_FILE 64

int file_cout = 1;

struct bootsector
{
	//* масиви
	char jump[3];
	char fsid[8];
	char pad[32];
	char bootcode[390];
	char bootsign[2];

	//* 64 розрядні поля
	uint64_t unk;
	uint64_t FsSizeInSector;

	//*32 розрядні поля
	//*сектори 	
	uint32_t sectorsToStart0Fat;
	uint32_t sectorsUsedForFat; 
	uint32_t sectorStart0FirstCluster;
	
	//*кластери
	uint32_t clusterCoinInFs;
	uint32_t clusterForRootDirectory;

	uint32_t punk[2];

	//*восьми розрядні поля
	uint8_t logBytesPerSector;
	uint8_t percentInUse;
	
	//* анк
	uint8_t dunk[7];
} __attribute__((packed));

uint32_t BootChecksum(const uint8_t * Sectors, uint16_t BytesPerSector)
{
	uint32_t NumberOfBytes = (uint32_t)BytesPerSector * 11;
	uint32_t checksum = 0;
	uint32_t Index = 0;

	for(Index = 0; Index < NumberOfBytes; Index++)
	{
		if((Index == 106) || (Index == 107) || (Index == 112))
		{
			continue;
		}

		checksum = ((checksum&1) ? 0x80000000 : 0) + (checksum>>1) + (uint32_t)Sectors[Index];
	}
	return checksum;
}

struct
{
	uint8_t entrytype;
	uint8_t entrycount;

	uint16_t checksum;
	uint16_t flags;
	uint16_t rvd;

	uint32_t creation, modification, aces;

	char pad[12];

} FileEntry = {.entrytype = 0x85};

struct
{
	//*восьми розрядні поля
	uint8_t entryType;
	uint8_t flags;
	uint8_t reserved;
	uint8_t filenameLengthInByte;
	
	//*шістнадцяти розрядні поля
	uint16_t filenameHash;
	uint16_t reserved2;

	//*шести-десяти-чотирьох розрядні поля 
	uint64_t valid_filesize;
	uint64_t file_size;

	//*тридцяти двох розрядні поля
	uint32_t reserved3;
	uint32_t startCluster;

} FileInfoEntry = {.entryType = 0xC0};

struct
{
	uint8_t entrystep;
	uint16_t name[15];

}fileNameEntry = {.entrystep = 0xc1};

//typedef struct{} FILE;

FS_FILE files[MAX_FILE] = {
	{"Readme.md", "Welcome to Merden!", STR_SIZE("Welcome to Merden")}};
const char *fs_read(const char *name ){
	for (int i = 0; i < file_cout; i++) 
	{
		if (strcmp(files[i].name, name) == 0)
		{
			return files[i].data;
		}
	}
	shell_print("File not found\n");
	return 0;
	}

void prints_file_info(char *name)
{
	for (int i = 0; i < file_cout; i++)
	{
		if (strcmp(files[i].name, name) == 0)
		{
			char size_str[64] = "Size: ";
			char buf[32];

			itoa(files[i].size, buf);
			strcat(size_str, buf);
			strcat(size_str, " bytes");
		}
	}
}

/*
FS_FILE *list = fs_get_files();

void fs_list()
{
	for (int i = 0; i < FILE_COUNT; i++)
	{
		shell_print((char *)files[i].name);
		shell_print("\n");
	}
}
*/

FS_FILE *fs_get_files(void)
{
	return files;
}

int fs_get_file_count(void)
{
	return file_cout;
}

char *fs_create(const char *name)
{
	strcpy(files[file_cout].name, name);
	files[file_cout].data[0] = '\0';
	files[file_cout].size = 0;

	file_cout += 1;
}

FS_FILE *fs_find(const char *name)
{
	for(int i = 0; file_cout > i; i++)
	{
		if(strcmp(files[i].name, name) == 0)
		{
			return &files[i];
		}
	}
	return 0;
}

FS_FILE *fs_get_or_create(const char *name)
{
	FS_FILE *exisisting = fs_find(name);
	if(exisisting)
	{
		return exisisting;
	}
	if (file_cout >= MAX_FILE)
	{
		return 0;
	}

	strcpy(files[file_cout].name, name);
	files[file_cout].data[0] = '\0';
	files[file_cout].size = 0;
	return &files[file_cout++];
}