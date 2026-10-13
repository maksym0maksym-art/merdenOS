#ifndef EXFAT_FS_H
#define EXFAT_FS_H

#include <stdint.h>
#include "blockdev.h"
#include "fat32.h"          /* спільні типи: fat_dirent_t, FAT_E_*, fat_part_info_t, fat_iter_cb, fat_path_join */

/*
 * exFAT поверх blockdev_t: читання, запис, створення, видалення, перейменування, каталоги.
 * API свідомо дзеркальний до fat32.h (префікс xf_ замість fat_), шляхи — абсолютні UTF-8
 * ("/docs/Звіт.txt"), без урахування регістру (за таблицею Up-case з тому).
 *
 * Обмеження: один том за раз; файли до 4 ГБ - 2; том до 2^32 секторів; не оновлюється VolumeFlags/
 * контрольна сума boot-регіону (прапорець "volume dirty" не ставиться); TexFAT не підтримується.
 */

#define XF_CACHE_LINES 8

typedef struct {
    int      valid, dirty;
    uint32_t sector;
    uint32_t stamp;
    uint8_t  data[FAT_SECTOR_MAX];
} xf_cache_t;

typedef struct {
    uint32_t first;            /* перший кластер (0 — порожньо) */
    uint32_t len;              /* довжина в байтах (DataLength) */
    uint8_t  nofat;            /* 1 — суміжний, ланцюжок у FAT не потрібен */
} xf_ref_t;

typedef struct {
    xf_ref_t ref;              /* потік каталогу */
    int      is_root;
    /* де лежить набір записів цього каталогу в батьківському (щоб оновлювати довжину при рості) */
    xf_ref_t pref;             /* потік батьківського каталогу */
    uint32_t pidx;             /* індекс 0x85-запису */
    int      proot;            /* батько — корінь */
} xf_dir_t;

typedef struct xf_vol {
    blockdev_t *bd;
    uint32_t part_lba;
    uint32_t bps, scale, spc, cluster_bytes;
    uint32_t fat_start, fat_size, nfats;
    uint32_t fat_extra_copies;                 /* лишено для сумісності з fat_vol_t у тестах */
    uint32_t heap_start;
    uint32_t root_cluster;
    uint32_t total_clusters;                   /* кластери 2 .. total_clusters+1 */
    uint32_t total_sectors;
    uint32_t bm_first, bm_len;                 /* Allocation Bitmap */
    uint8_t  bm_nofat;
    uint32_t free_count, next_free;
    uint32_t root_len;
    int      read_only;
    char     label[12 * 4 + 1];
    uint32_t serial;
    uint32_t stamp;
    xf_cache_t cache[XF_CACHE_LINES];
} xf_vol_t;

typedef struct {
    xf_vol_t   *v;
    fat_dirent_t ent;
    xf_dir_t    parent;                        /* каталог, де лежить запис */
    uint32_t    set_idx, set_sc;               /* 0x85-запис і кількість вторинних */
    uint8_t     flags;                         /* General Secondary Flags потоку */
    uint32_t    size, first_cluster, dl;       /* VDL, перший кластер, DataLength */
    uint8_t     nofat;
    uint32_t    cur_idx, cur_cl;
    int         dirty, writable;
} xf_file_t;

int  xf_mount(xf_vol_t *v, blockdev_t *bd, int part_index, fat_part_info_t *info);
int  xf_flush(xf_vol_t *v);
void xf_set_clock(void (*get)(uint16_t *date, uint16_t *time));
int  xf_statfs(xf_vol_t *v, uint32_t *total_clusters, uint32_t *free_clusters, uint32_t *cluster_bytes);

int  xf_stat(xf_vol_t *v, const char *path, fat_dirent_t *out);
int  xf_readdir(xf_vol_t *v, const char *path, fat_iter_cb cb, void *arg);

int  xf_open(xf_vol_t *v, const char *path, int create, xf_file_t *f);
int  xf_fread(xf_file_t *f, uint32_t off, void *buf, uint32_t n);
int  xf_fwrite(xf_file_t *f, uint32_t off, const void *buf, uint32_t n);
int  xf_ftruncate(xf_file_t *f, uint32_t newsize);
int  xf_fclose(xf_file_t *f);

int  xf_read_file(xf_vol_t *v, const char *path, void *buf, uint32_t cap, uint32_t *out_len);
int  xf_write_file(xf_vol_t *v, const char *path, const void *buf, uint32_t n);
int  xf_append_file(xf_vol_t *v, const char *path, const void *buf, uint32_t n);
int  xf_create(xf_vol_t *v, const char *path);
int  xf_mkdir(xf_vol_t *v, const char *path);
int  xf_unlink(xf_vol_t *v, const char *path);
int  xf_rmdir(xf_vol_t *v, const char *path);
int  xf_rename(xf_vol_t *v, const char *from, const char *to);

#endif
