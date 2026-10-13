#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/*
 * Найпростіший блоковий пристрій: сектори фіксованого розміру, 32-бітні LBA
 * (до 2 ТБ при 512 Б, до 16 ТБ при 4 КБ). Усі функції повертають 0 при успіху.
 */
typedef struct blockdev {
    void     *ctx;
    uint32_t  block_size;
    uint32_t  block_count;
    int     (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int     (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    int     (*flush)(void *ctx);
    int       read_only;
} blockdev_t;

#endif
