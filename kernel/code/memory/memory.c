
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../headers/system.h"
#include "../../headers/kernel-panic.h"
#include "../../headers/memory.h"

#define PAGE_SIZE       4096u
#define HEAP_SIZE       (8192u * PAGE_SIZE)  /* 32 МіБ: стеки задач, файл PNG і розпакований кадр беруться звідси */
#define ALIGNMENT       16u
#define BLOCK_MAGIC     0x4D45524Du

typedef struct block_meta {
    size_t size;
    bool is_free;
    uint32_t magic;
    struct block_meta *next;
} block_meta_t;

#define META_SIZE \
    ((sizeof(block_meta_t) + ALIGNMENT - 1u) & ~(ALIGNMENT - 1u))

/* Тимчасова купа: 32 МіБ (у .bss), вирівняна на 16 байтів. */
static uint8_t heap_storage[HEAP_SIZE]
    __attribute__((aligned(16)));

static block_meta_t *heap_head = NULL;
static bool heap_initialized = false;

/* Вирівнювання з перевіркою переповнення. */
static bool align_size(size_t size, size_t *result)
{
    if (size > SIZE_MAX - (ALIGNMENT - 1u))
        return false;

    *result = (size + ALIGNMENT - 1u)
            & ~(size_t)(ALIGNMENT - 1u);

    return true;
}

void kheap_init(void)
{
    if (heap_initialized)
        return;

    heap_head = (block_meta_t *)heap_storage;

    heap_head->size = HEAP_SIZE - META_SIZE;
    heap_head->is_free = true;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->next = NULL;

    heap_initialized = true;
}

static void split_block(block_meta_t *block, size_t size)
{
    /* Викликати лише тоді, коли залишиться повноцінний блок. */
    if (block->size < size ||
        block->size - size < META_SIZE + ALIGNMENT)
        return;

    block_meta_t *new_block = (block_meta_t *)
        ((uint8_t *)block + META_SIZE + size);

    new_block->size = block->size - size - META_SIZE;
    new_block->is_free = true;
    new_block->magic = BLOCK_MAGIC;
    new_block->next = block->next;

    block->size = size;
    block->next = new_block;
}

void *kmalloc(size_t size)
{
    if (!heap_initialized || size == 0)
        return NULL;

    if (!align_size(size, &size))
        return NULL;

    for (block_meta_t *block = heap_head;
         block != NULL;
         block = block->next)
    {
        if (block->magic != BLOCK_MAGIC)
            return NULL;

        if (!block->is_free || block->size < size)
            continue;

        split_block(block, size);
        block->is_free = false;

        return (uint8_t *)block + META_SIZE;
    }

    /* У цій версії купа фіксованого розміру. */
    return NULL;
}

void kfree(void *ptr)
{
    if (ptr == NULL || !heap_initialized)
        return;

    block_meta_t *previous = NULL;
    block_meta_t *block = heap_head;

    /*
     * Шукаємо точну адресу виділеного блока.
     * Не читаємо метадані за довільною адресою ptr.
     */
    while (block != NULL)
    {
        if (block->magic != BLOCK_MAGIC)
            return;

        if ((uint8_t *)block + META_SIZE == (uint8_t *)ptr)
            break;

        previous = block;
        block = block->next;
    }

    /* Чужий pointer або pointer усередину блока. */
    if (block == NULL || block->is_free)
        return;

    block->is_free = true;

    /* Об'єднання із наступними вільними блоками. */
    while (block->next != NULL &&
           block->next->magic == BLOCK_MAGIC &&
           block->next->is_free)
    {
        block_meta_t *next = block->next;

        block->size += META_SIZE + next->size;
        block->next = next->next;
    }

    /* Об'єднання з попереднім блоком, якщо він вільний. */
    if (previous != NULL && previous->is_free)
    {
        previous->size += META_SIZE + block->size;
        previous->next = block->next;
    }
}
