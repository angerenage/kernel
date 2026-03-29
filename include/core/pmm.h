#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PMM_PAGE_SIZE 0x1000ull

bool pmm_init(void);

bool pmm_alloc_pages(size_t count, uintptr_t* out_phys);
bool pmm_free_pages(uintptr_t phys, size_t count);

size_t pmm_managed_range_count(void);
size_t pmm_total_page_count(void);
size_t pmm_free_page_count(void);
