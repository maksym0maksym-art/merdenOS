#include "../headers/EXFAT.h"
#include "../../standarts_lib/stdio.h"
#include "../headers/kernel-panic.h"
#include "../headers/vga.h"
#include "../headers/AHCI.h"
#define STR_SIZE(str) (sizeof(str) - 1)
#define FILE_COUNT (sizeof(files) / sizeof(files[0]))


int memcmp(const void *a, const void *b, unsigned int count)
{
    const unsigned char *p1 = (const unsigned char *)a;
    const unsigned char *p2 = (const unsigned char *)b;

    while (count--)
    {
        if (*p1 != *p2)
        {
            return *p1 - *p2;
        }

        p1++;
        p2++;
    }

    return 0;
}

typedef struct __attribute__((packed))
{
    uint8_t EntryType;
    uint8_t GeneralFlags;
    uint8_t Reserved1;
    uint8_t NameLength;

    uint16_t NameHash;
    uint16_t Reserved2;

    uint64_t ValidDataLength;

    uint32_t Reserved3;
    uint32_t FirstCluster;

    uint64_t DataLength;

} ExFatStreamEntry;

#define MAX_FILE 64

int file_cout = 1;

typedef struct __attribute__((packed))
{
    uint8_t JumpBoot[3];
    char FileSystemName[8];          // "EXFAT   "
    uint8_t MustBeZero[53];

    uint64_t PartitionOffset;
    uint64_t VolumeLength;

    uint32_t FatOffset;
    uint32_t FatLength;

    uint32_t ClusterHeapOffset;
    uint32_t ClusterCount;
    uint32_t FirstClusterOfRootDirectory;

    uint32_t VolumeSerialNumber;

    uint16_t FileSystemRevision;
    uint16_t VolumeFlags;

    uint8_t BytesPerSectorShift;
    uint8_t SectorsPerClusterShift;
    uint8_t NumberOfFats;
    uint8_t DriveSelect;
    uint8_t PercentInUse;

    uint8_t Reserved[7];

    uint8_t BootCode[390];

    uint16_t BootSignature;          // 0xAA55

}	bootsector;

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


static bootsector boot;

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
	kernel_panic(FOS_ERROR_UNKNOWN);
	asm volatile("hlt");
	return 0;
	}

typedef struct
{
    char name[256];

    uint32_t first_cluster;

    uint64_t size;

} EXFAT_FILE;

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

            t_printf("%s\n", size_str);

            uint32_t bytes_per_sector = 1 << boot.BytesPerSectorShift;
            uint32_t sectors_per_cluster = 1 << boot.SectorsPerClusterShift;
            uint32_t cluster_size = bytes_per_sector * sectors_per_cluster;

            t_printf("Bytes/sector: %d\n", bytes_per_sector);
			t_printf("Sectors/cluster: %d\n", sectors_per_cluster);
			t_printf("Cluster size: %d\n", bytes_per_sector * sectors_per_cluster);
			t_printf("Root cluster: %d\n", boot.FirstClusterOfRootDirectory);
			t_printf("FAT offset: %d\n", boot.FatOffset);
			t_printf("Cluster heap: %d\n", boot.ClusterHeapOffset);
        }
    }
}

int exfat_read_cluster(
    HBA_PORT *port,
    uint32_t cluster,
    void *buffer
);

#define EXFAT_CLUSTER_END 0xFFFFFFFF

uint32_t exfat_allocate_cluster(HBA_PORT *port)
{
    uint32_t bytes_per_sector = 1 << boot.BytesPerSectorShift;

    uint32_t buffer[512];

    for (uint32_t sector = 0; sector < boot.FatLength; sector++)
    {
        if (ahci_read(port, boot.FatOffset + sector, 1, buffer))
        {
            return 0;
        }

        uint32_t *fat = (uint32_t *)buffer;
        uint32_t entries = bytes_per_sector / 4;   

        for (int i = 0; i < entries; i++)
        {
            uint32_t cluster = sector  * entries + i; 

            if(cluster < 2)
            {
                continue; 
            }

            if (fat[i] == 0)
            {
                fat[i] = EXFAT_CLUSTER_END;

                if(ahci_write(
                    port,
                    boot.FatOffset + sector,
                    1,
                    buffer
                ))
                {
                  return 0;  
                }
                return cluster;
            }

        }
    }
    return 0;
}

typedef struct __attribute__((packed))
{
    uint8_t EntryType;
    uint8_t GeneralFlags;
    uint16_t FileName[15];

} ExFatFileNameEntry;

int exfat_create_file(HBA_PORT *port, const char *name)
{
    uint32_t cluster = exfat_allocate_cluster(port);

    uint32_t bytes_per_sector = 1 << boot.BytesPerSectorShift;

    if (cluster == 0)
    {
        t_printf("empty_cluster");
        t_printf("FAT length: %d\n", boot.FatLength);
        t_printf("Bytes/sector: %d\n", bytes_per_sector);
        return -1;
    }

    uint32_t sectors_per_cluster = 1 << boot.SectorsPerClusterShift;

    uint64_t root_lba =
    boot.ClusterHeapOffset +
    ((boot.FirstClusterOfRootDirectory - 2) * sectors_per_cluster);

    uint8_t buffer[512];

    if (cluster == 0)
    {
        t_printf("Cannot alocate cluster\n");
        return -1;
    }

    t_printf("Alocated cluster: ");
    t_print_int(cluster);
    t_printf("\n");

    if (ahci_read(port, root_lba, sectors_per_cluster, buffer))
    {
        t_printf("Cannot read root directory \n");
        return -1;
    }


    uint8_t *ptr = buffer;
    uint8_t *end = buffer + sectors_per_cluster * 512;

    while (ptr < end)
    {
        if (ptr[0] == 0x00)
            break;

        ptr += 32;
    }

    if (ptr >= end)
    {
        t_printf("No free directory entry\n");
        return -1;
    }

    while(1)
    {
        if (ptr[0] == 0x00)
        {
            t_printf("Root directory found successfully \n");
            break;
        }

        ptr += 32;
    }

    memset(ptr, 0, 32);

    ptr[0] = 0x85;
    ptr[1] = 2;

    ptr += 32;

    ExFatStreamEntry *stream = (ExFatStreamEntry *) ptr;

    memset(stream, 0, sizeof(ExFatStreamEntry));
    
    stream->EntryType = 0xC0;
    stream->NameLength = strlen(name);

    stream->FirstCluster = cluster;

    stream->DataLength = 0;
    stream->ValidDataLength = 0;

    ptr += 32;

    ExFatFileNameEntry *fname = (ExFatFileNameEntry *)ptr;
    memset(fname, 0, sizeof(ExFatFileNameEntry));

    fname->EntryType = 0xC1;

    for (int i = 0; i < 15 && name[i]; i++)
    {
        fname->FileName[i] = name[i];
    }

    if (ahci_write(port, root_lba, sectors_per_cluster, buffer))
    {
        t_printf("Cannot write root directory:(\n");
        return -1;
    }

    return 0;

}

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

/*int exfat_mount(HBA_PORT *port)
{
    if (ahci_read(port, 0, 1, &boot))
        return -1;


    if (memcmp(boot.FileSystemName, "EXFAT   ", 8) != 0)
        return -1;
        t_printf("OEM: %.8s\n", boot.FileSystemName);
        t_printf("BytesPerSectorShift: %d\n", boot.BytesPerSectorShift);
        t_printf("SectorsPerClusterShift: %d\n", boot.SectorsPerClusterShift);
        t_printf("FatOffset: %d\n", boot.FatOffset);
        t_printf("FatLength: %d\n", boot.FatLength);
        t_printf("ClusterHeapOffset: %d\n", boot.ClusterHeapOffset);
        t_printf("RootCluster: %d\n", boot.FirstClusterOfRootDirectory);


	uint32_t sector_per_cluster = 1 << boot.SectorsPerClusterShift;

	uint64_t root_lba =
	boot.ClusterHeapOffset +
	((boot.FirstClusterOfRootDirectory - 2) * sector_per_cluster);
	
	/* 
	*кластер 0 — не існує
	*кластер 1 — не існує
	*кластер 2 — перший реальний кластер	


	uint8_t buffer[65536];

	if (ahci_read(
		port,
		root_lba,
		sector_per_cluster,
		buffer
	))
	{
		t_printf("Cannot to read this directory :(");
	}	

	uint8_t *ptr = buffer;

	EXFAT_FILE file;

	int name_pos = 0;

	while (1)
{
    uint8_t type = ptr[0];

    if (type == 0x00)
        break;

    switch(type)
    {
        case 0x85:

		if (file.name[0] != '\0')
    	{
        	t_printf("File: %s\n", file.name);
        	t_printf("Cluster: %d\n", file.first_cluster);
        	t_printf("Size: %d\n", (uint32_t)file.size);
    	}

			memset(&file, 0, sizeof(file));
    		name_pos = 0;
            break;

        case 0xC0:
        {
            ExFatStreamEntry *stream = (ExFatStreamEntry *)ptr;

            file.first_cluster = stream->FirstCluster;
            file.size = stream->DataLength;
            break;
        }

        case 0xC1:
        {
            ExFatFileNameEntry *name = (ExFatFileNameEntry *)ptr;

			

			for (int i = 0; i < 15; i++)
			{
				if (name->FileName[i] == 0)
					break;

				file.name[name_pos++] = (char)name->FileName[i];
			}

			file.name[name_pos] = '\0';

            // дописуємо 15 символів у file.name
            break;
        }
    }

    ptr += 32;
}

	if (file.name[0] != '\0')
	{
    	t_printf("File: %s\n", file.name);
		
		uint8_t file_buffer[65536];

		if (exfat_read_cluster(port,
							file.first_cluster,
							file_buffer) == 0)
		{
			t_printf("First byte: %x\n", file_buffer[0]);
		}

    	t_printf("Cluster: %d\n", file.first_cluster);
    	t_printf("Size: %d\n", (uint32_t)file.size);
	}

	return 0;
}
*/

int exfat_mount(HBA_PORT *port)
{
    if (ahci_read(port, 0, 1, &boot))
    {
        t_printf("ahci_read FAILED\n");
        return -1;
    }

    uint8_t *p = (uint8_t *)&boot;

    t_printf("First bytes: ");

    for (int i = 0; i < 16; i++)
    {
        t_printf("%x ", p[i]);
    }

    t_printf("\n");

    t_printf("OEM: %.8s\n", boot.FileSystemName);

    if (memcmp(boot.FileSystemName, "EXFAT   ", 8) != 0)
    {
        t_printf("Not EXFAT!\n");
        return -1;
    }


    }

int exfat_read_cluster(HBA_PORT *port,
                       uint32_t cluster,
                       void *buffer)
{
    uint32_t sectors_per_cluster =
        1 << boot.SectorsPerClusterShift;

    uint64_t lba =
        boot.ClusterHeapOffset +
        ((cluster - 2) * sectors_per_cluster);

    return ahci_read(
        port,
        lba,
        sectors_per_cluster,
        buffer
    );
}

uint32_t exfat_next_cluster(HBA_PORT *port, uint32_t cluster)
{
    uint32_t bytes_per_sector =
        1 << boot.BytesPerSectorShift;

    uint32_t fat_offset = cluster * 4;

    uint64_t sector =
        boot.FatOffset +
        (fat_offset / bytes_per_sector);

    uint32_t offset =
        fat_offset % bytes_per_sector;

    uint8_t buffer[4096];

    if (ahci_read(port, sector, 1, buffer))
        return 0xFFFFFFFF;

    return *(uint32_t *)(buffer + offset);
}
