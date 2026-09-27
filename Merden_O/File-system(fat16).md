[[File-system(fat16)]]
[[File-system(global_makros)]]
[[налаштування_файлових_систем]]
#Функція 
код:
```c
unsigned char FAT_table[sector_size];
unsigned int fat_offset = active_cluster * 2;
unsigned int fat_sector = first_fat_sector + (fat_offset / sector_size);
unsigned int ent_offset = fat_offset % sector_size;

//at this point you need to read from sector "fat_sector" on the disk into "FAT_table".

unsigned short table_value = *(unsigned short*)&FAT_table[ent_offset];

//the variable "table_value" now has the information you need about the next cluster in the chain.
```
