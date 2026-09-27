[[FIle-system(fat12)]]
додаткові макроси у [[File-system(global_makros)]]
**код** знизу

```c
unsigned char FAT_table[sector_size * 2]; // needs two in case we straddle a sector
unsigned int fat_offset = active_cluster + (active_cluster / 2);// multiply by 1.5
unsigned int fat_sector = first_fat_sector + (fat_offset / sector_size);
unsigned int ent_offset = fat_offset % sector_size;

//at this point you need to read two sectors from disk starting at "fat_sector" into "FAT_table".

unsigned short table_value = *(unsigned short*)&FAT_table[ent_offset];

table_value = (active_cluster & 1) ? table_value >> 4 : table_value & 0xfff;

```
**також системи**
[[File-system(fat16)]]