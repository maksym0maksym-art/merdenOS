#ifndef MERDEN_MEMORY_H
#define MERDEN_MEMORY_H

#include <stddef.h>

void kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif