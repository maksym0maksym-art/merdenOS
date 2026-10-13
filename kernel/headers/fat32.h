#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "blockdev.h"

/*
 * FAT32 з довгими іменами (LFN): читання, запис, створення, видалення, перейменування,
 * каталоги. Працює поверх blockdev_t (USB-флешка, IDE, образ файлу в host-тесті).
 *
 * Шляхи — абсолютні, з '/', у UTF-8, без урахування регістру: "/docs/ReadMe.txt".
 * Вся метадана пишеться через невеликий кеш секторів; кожна публічна операція, що щось
 * змінює, закінчується fat_flush(), тому після повернення з функції флешку можна виймати
 * (якщо драйвер USB зробив SYNCHRONIZE CACHE — це робить usb_msc_sync через bd->flush).
 *
 * Обмеження: один том за раз; імена до 255 байт UTF-8; розмір файлу до 4 ГБ - 1;
 * без підтримки сторінки кодування OEM для коротких імен (байти > 0x7F читаються як Latin-1).
 */

#define FAT_NAME_MAX      255

#define FAT_OK              0
#define FAT_E_IO          (-1)
#define FAT_E_NOENT       (-2)    /* не знайдено */
#define FAT_E_EXIST       (-3)    /* вже існує */
#define FAT_E_NOTDIR      (-4)
#define FAT_E_ISDIR       (-5)
#define FAT_E_NOSPC       (-6)    /* нема місця */
#define FAT_E_NOTEMPTY    (-7)    /* каталог не порожній */
#define FAT_E_INVAL       (-8)    /* некоректне ім'я/аргумент */
#define FAT_E_FMT         (-9)    /* не FAT32 або файлова система пошкоджена */
#define FAT_E_RO          (-10)   /* носій тільки для читання / файл має атрибут read-only */
#define FAT_E_NAMETOOLONG (-11)

#define FAT_ATTR_RO       0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYSTEM   0x04
#define FAT_ATTR_VOLUME   0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20
#define FAT_ATTR_LFN      0x0F

#define FAT_SECTOR_MAX    4096
#define FAT_CACHE_LINES   8

typedef struct {
    char     name[FAT_NAME_MAX + 1];       /* UTF-8 */
    uint8_t  attr;
    uint32_t size;
    uint32_t first_cluster;
    uint16_t cdate, ctime, wdate, wtime, adate;
    /* де лежить запис (для зміни/видалення) */
    uint32_t dir_cluster;                  /* перший кластер батьківського каталогу (0 — це корінь, у якого нема батька) */
    uint32_t ent_idx;                      /* індекс короткого запису в каталозі */
    uint32_t lfn_idx;                      /* індекс першого LFN-запису (== ent_idx, якщо LFN нема) */
} fat_dirent_t;

typedef struct {
    int      valid, dirty;
    uint32_t sector;                       /* номер сектора тому */
    uint32_t stamp;
    uint8_t  data[FAT_SECTOR_MAX];
} fat_cache_t;

typedef struct fat_vol {
    blockdev_t *bd;
    uint32_t part_lba;                     /* початок тому в блоках пристрою */
    uint32_t bps, scale, spc, cluster_bytes;
    uint32_t reserved, nfats, fat_size, total_sectors;
    uint32_t fat_start;                    /* сектор активної FAT */
    uint32_t fat_extra_copies;             /* скільки ще копій FAT дзеркалити при записі */
    uint32_t data_start;
    uint32_t root_cluster;
    uint32_t total_clusters;               /* кластери даних: номери 2 .. total_clusters+1 */
    uint32_t fsinfo_sector;                /* 0 — нема */
    uint32_t free_count, next_free;        /* 0xFFFFFFFF — невідомо */
    int      fsinfo_dirty;
    int      read_only;
    char     label[12];
    uint32_t serial;
    uint32_t stamp;
    fat_cache_t cache[FAT_CACHE_LINES];
} fat_vol_t;

typedef struct {
    fat_vol_t   *v;
    fat_dirent_t ent;                      /* запис у каталозі (для оновлення розміру/кластера) */
    uint32_t     size, first_cluster;
    uint32_t     cur_idx, cur_cl;          /* курсор по ланцюжку: кластер із номером cur_idx в файлі */
    int          dirty;
    int          writable;
} fat_file_t;

/* Інформація про розділи, що знайшов fat_mount (для `usb info`) */
typedef struct {
    int      scheme;                       /* 0 — без таблиці, 1 — MBR, 2 — GPT */
    uint32_t start_lba;
    uint32_t sectors;
} fat_part_info_t;

/* --- том --- */
int  fat_mount(fat_vol_t *v, blockdev_t *bd, int part_index /* -1 — перший FAT32 */, fat_part_info_t *info);
int  fat_flush(fat_vol_t *v);
void fat_set_clock(void (*get)(uint16_t *date, uint16_t *time));
int  fat_statfs(fat_vol_t *v, uint32_t *total_clusters, uint32_t *free_clusters, uint32_t *cluster_bytes);

/* --- шляхи --- */
/* cwd + arg -> нормалізований абсолютний шлях ('.' і '..' розкриті). 0 або FAT_E_INVAL/NAMETOOLONG. */
int  fat_path_join(const char *cwd, const char *arg, char *out, int outsz);

/* --- інформація --- */
int  fat_stat(fat_vol_t *v, const char *path, fat_dirent_t *out);
typedef int (*fat_iter_cb)(const fat_dirent_t *e, void *arg);       /* ненульове значення — зупинити */
int  fat_readdir(fat_vol_t *v, const char *path, fat_iter_cb cb, void *arg);

/* --- файли --- */
int  fat_open(fat_vol_t *v, const char *path, int create, fat_file_t *f);
int  fat_fread(fat_file_t *f, uint32_t off, void *buf, uint32_t n);          /* прочитано байт або <0 */
int  fat_fwrite(fat_file_t *f, uint32_t off, const void *buf, uint32_t n);   /* записано байт або <0 */
int  fat_ftruncate(fat_file_t *f, uint32_t newsize);
int  fat_fclose(fat_file_t *f);                                              /* оновлює запис у каталозі + flush */

/* --- зручні обгортки --- */
int  fat_read_file(fat_vol_t *v, const char *path, void *buf, uint32_t cap, uint32_t *out_len);
int  fat_write_file(fat_vol_t *v, const char *path, const void *buf, uint32_t n);   /* створює або замінює вміст */
int  fat_append_file(fat_vol_t *v, const char *path, const void *buf, uint32_t n);  /* створює, якщо нема */
int  fat_create(fat_vol_t *v, const char *path);                                    /* порожній файл; FAT_E_EXIST, якщо є */
int  fat_mkdir(fat_vol_t *v, const char *path);
int  fat_unlink(fat_vol_t *v, const char *path);
int  fat_rmdir(fat_vol_t *v, const char *path);
int  fat_rename(fat_vol_t *v, const char *from, const char *to);

const char *fat_strerror(int err);

#endif
