#include "../../headers/PMM.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE       4096u
#define PAGE_SHIFT      12u

//* 32-бітний адресний простір: максимум 4 ГіБ. 
#define PMM_MAX_PAGES   1048576u
#define BITMAP_SIZE     (PMM_MAX_PAGES / 8u)
#define PHYS_LIMIT      0x100000000ULL

#define MB2_TAG_END     0u
#define MB2_TAG_MODULE  3u
#define MB2_TAG_MMAP    6u
#define MB2_TAG_FB      8u

#define MB2_MAGIC       0x36D76289u

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} mb2_mmap_tag_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} mb2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
} mb2_module_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t fb_type;
    uint16_t reserved;
} mb2_fb_tag_t;

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

/* 1 = зайнята сторінка, 0 = вільна. */
static uint8_t used_bitmap[BITMAP_SIZE];

/* Не дозволяємо звільняти зарезервовані сторінки. */
static uint8_t reserved_bitmap[BITMAP_SIZE];

static uint32_t free_pages;
static bool initialized;

static bool bit_get(const uint8_t *map, uint32_t page)
{
    return (map[page >> 3] &
            (uint8_t)(1u << (page & 7u))) != 0;
}

static void bit_set(uint8_t *map, uint32_t page)
{
    map[page >> 3] |=
        (uint8_t)(1u << (page & 7u));
}

static void bit_clear(uint8_t *map, uint32_t page)
{
    map[page >> 3] &=
        (uint8_t)~(1u << (page & 7u));
}

static void reserve_range(uint64_t start, uint64_t end)
{
    if (start >= PHYS_LIMIT || end <= start)
        return;

    if (end > PHYS_LIMIT)
        end = PHYS_LIMIT;

    uint32_t first = (uint32_t)(start / PAGE_SIZE);
    uint32_t last = (uint32_t)(
        (end + PAGE_SIZE - 1u) / PAGE_SIZE
    );

    if (last > PMM_MAX_PAGES)
        last = PMM_MAX_PAGES;

    for (uint32_t p = first; p < last; p++) {
        if (!bit_get(used_bitmap, p))
            free_pages--;

        bit_set(used_bitmap, p);
        bit_set(reserved_bitmap, p);
    }
}

static void make_available(uint64_t start, uint64_t length)
{
    if (start >= PHYS_LIMIT || length == 0)
        return;

    uint64_t end;

    if (length > PHYS_LIMIT - start)
        end = PHYS_LIMIT;
    else
        end = start + length;

    /* Використовуємо тільки повністю доступні сторінки. */
    uint64_t first = (start + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t last = end / PAGE_SIZE;

    if (last > PMM_MAX_PAGES)
        last = PMM_MAX_PAGES;

    for (uint64_t p = first; p < last; p++) {
        uint32_t page = (uint32_t)p;

        if (bit_get(reserved_bitmap, page))
            continue;

        if (bit_get(used_bitmap, page)) {
            bit_clear(used_bitmap, page);
            free_pages++;
        }
    }
}

bool pmm_init(uint32_t mbi_addr)
{
    if (initialized)
        return true;

    free_pages = 0;

    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        used_bitmap[i] = 0xFF;
        reserved_bitmap[i] = 0;
    }

    if (mbi_addr == 0)
        return false;

    const mb2_info_t *info =
        (const mb2_info_t *)(uintptr_t)mbi_addr;

    uint32_t total_size = info->total_size;

    if (total_size < 16u ||
        (uint64_t)mbi_addr + total_size > PHYS_LIMIT)
        return false;

    const uint8_t *base =
        (const uint8_t *)(uintptr_t)mbi_addr;

    const mb2_mmap_tag_t *mmap = NULL;
    uint32_t offset = 8u;

    /* Знаходимо карту фізичної пам'яті Multiboot2. */
    while (offset <= total_size - 8u) {
        const mb2_tag_t *tag =
            (const mb2_tag_t *)(base + offset);

        if (tag->size < 8u ||
            tag->size > total_size - offset)
            return false;

        if (tag->type == MB2_TAG_MMAP) {
            if (tag->size < 16u)
                return false;

            mmap = (const mb2_mmap_tag_t *)tag;
            break;
        }

        if (tag->type == MB2_TAG_END)
            break;

        uint64_t next =
            ((uint64_t)offset + tag->size + 7u)
            & ~7ULL;

        if (next > total_size)
            return false;

        offset = (uint32_t)next;
    }

    if (mmap == NULL ||
        mmap->entry_size < sizeof(mb2_mmap_entry_t) ||
        (mmap->size - 16u) % mmap->entry_size != 0)
        return false;

    /* type == 1 означає доступну RAM. */
    const uint8_t *entry_ptr =
        (const uint8_t *)mmap + 16u;

    uint32_t entry_count =
        (mmap->size - 16u) / mmap->entry_size;

    for (uint32_t i = 0; i < entry_count; i++) {
        const mb2_mmap_entry_t *entry =
            (const mb2_mmap_entry_t *)(
                entry_ptr + (uint64_t)i * mmap->entry_size
            );

        if (entry->type == 1u)
            make_available(entry->addr, entry->len);
    }

    /*
     * Низька пам'ять містить BIOS-області та інші
     * структури, які не можна видавати ядру.
     */
    reserve_range(0, 0x100000u);

    /* Резервуємо все ядро, включно зі стеком і bitmap. */
    reserve_range(
        (uintptr_t)&__kernel_start,
        (uintptr_t)&__kernel_end
    );

    /* Резервуємо інформаційну структуру GRUB. */
    reserve_range(mbi_addr, (uint64_t)mbi_addr + total_size);

    /*
     * Модулі завантажувача та framebuffer також можуть
     * займати фізичну пам'ять. Знаходимо їхні теги.
     */
    offset = 8u;

    while (offset <= total_size - 8u) {
        const mb2_tag_t *tag =
            (const mb2_tag_t *)(base + offset);

        if (tag->size < 8u ||
            tag->size > total_size - offset)
            return false;

        if (tag->type == MB2_TAG_MODULE &&
            tag->size >= sizeof(mb2_module_tag_t)) {
            const mb2_module_tag_t *mod =
                (const mb2_module_tag_t *)tag;

            if (mod->mod_end >= mod->mod_start)
                reserve_range(mod->mod_start, mod->mod_end);
        }

        if (tag->type == MB2_TAG_FB &&
            tag->size >= sizeof(mb2_fb_tag_t)) {
            const mb2_fb_tag_t *fb =
                (const mb2_fb_tag_t *)tag;

            uint64_t fb_size =
                (uint64_t)fb->pitch * fb->height;

            reserve_range(fb->addr, fb->addr + fb_size);
        }

        if (tag->type == MB2_TAG_END)
            break;

        uint64_t next =
            ((uint64_t)offset + tag->size + 7u)
            & ~7ULL;

        if (next > total_size)
            return false;

        offset = (uint32_t)next;
    }

    initialized = true;
    return true;
}

void *pmm_alloc_page(void)
{
    if (!initialized || free_pages == 0)
        return NULL;

    for (uint32_t page = 256u;
         page < PMM_MAX_PAGES;
         page++) {
        if (!bit_get(used_bitmap, page)) {
            bit_set(used_bitmap, page);
            free_pages--;

            return (void *)(uintptr_t)(page << PAGE_SHIFT);
        }
    }

    return NULL;
}

void pmm_free_page(void *ptr)
{
    if (!initialized || ptr == NULL)
        return;

    uintptr_t addr = (uintptr_t)ptr;

    if ((addr & (PAGE_SIZE - 1u)) != 0)
        return;

    uint32_t page = (uint32_t)(addr >> PAGE_SHIFT);

    if (page >= PMM_MAX_PAGES)
        return;

    if (bit_get(reserved_bitmap, page))
        return;

    if (!bit_get(used_bitmap, page))
        return;

    bit_clear(used_bitmap, page);
    free_pages++;
}

uint32_t pmm_get_free_pages(void)
{
    return initialized ? free_pages : 0;
}