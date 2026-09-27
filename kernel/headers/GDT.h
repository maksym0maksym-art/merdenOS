#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GDT
{
    uint32_t base;
    uint32_t limit;
    uint8_t access_byte;
    uint8_t flags;
};

void encode_Gdt_entry(uint8_t *target, struct GDT source);

uint64_t create_descriptor(uint32_t base, uint32_t limit, uint16_t flag);

int main_g(void); 

#define GDT_ACCESS_ACCESSED   (1 << 0)

#define GDT_ACCESS_RW         (1 << 1)

#define GDT_ACCESS_DC         (1 << 2)

#define GDT_ACCESS_EXEC       (1 << 3)

#define GDT_ACCESS_TYPE       (1 << 4)

#define GDT_ACCESS_RING0      (0 << 5)
#define GDT_ACCESS_RING1      (1 << 5)
#define GDT_ACCESS_RING2      (2 << 5)
#define GDT_ACCESS_RING3      (3 << 5)

#define GDT_ACCESS_PRESENT    (1 << 7)


#define GDT_CODE_ACCESS_PL0 ( \
    GDT_ACCESS_PRESENT | \
    GDT_ACCESS_TYPE | \
    GDT_ACCESS_EXEC | \
    GDT_ACCESS_RW | \
    GDT_ACCESS_RING0 \
)

#define GDT_DATA_ACCESS_PL0 ( \
    GDT_ACCESS_PRESENT | \
    GDT_ACCESS_TYPE | \
    GDT_ACCESS_RW | \
    GDT_ACCESS_RING0 \
)
#define GDT_DATA_ACCESS_PL3 ( \
    GDT_ACCESS_PRESENT | \
    GDT_ACCESS_TYPE | \
    GDT_ACCESS_RW | \
    GDT_ACCESS_RING3 \
)
#define GDT_CODE_ACCESS_PL3 ( \
    GDT_ACCESS_PRESENT | \
    GDT_ACCESS_TYPE | \
    GDT_ACCESS_EXEC | \
    GDT_ACCESS_RW | \
    GDT_ACCESS_RING3 \
)

#define GDT_FLAG_32BIT   (1 << 6)
#define GDT_FLAG_GRAN    (1 << 7)

#define GDT_FLAGS ( \
    GDT_FLAG_32BIT | \
    GDT_FLAG_GRAN \
)


#ifdef __cplusplus
}
#endif

#endif // GDT_H