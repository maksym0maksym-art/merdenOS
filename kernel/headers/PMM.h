
#ifndef MERDEN_PMM_H
#define MERDEN_PMM_H

#include <stdint.h>
#include <stdbool.h>

bool pmm_init(uint32_t mbi_addr);
void *pmm_alloc_page(void);
void pmm_free_page(void *page);
uint32_t pmm_get_free_pages(void);

#endif