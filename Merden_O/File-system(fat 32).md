[[File-system(fat 32)]]
[[File-system(fat16)]]
[[FIle-system(fat12)]]
[[File-system(global_makros)]]
[[налаштування_файлових_систем]]

```c
unsigned char FAT_table[sector_size];
unsigned int fat_offset = active_cluster * 4;
unsigned int fat_sector = first_fat_sector + (fat_offset / sector_size);
unsigned int ent_offset = fat_offset % sector_size;

//at this point you need to read from sector "fat_sector" on the disk into "FAT_table".

//remember to ignore the high 4 bits.
unsigned int table_value = *(unsigned int*)&FAT_table[ent_offset];
if (fat32) table_value &= 0x0FFFFF

```