#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool vaddr_alloc_init(uintptr_t base, size_t page_count);
bool vaddr_alloc_is_initialized(void);

bool vaddr_alloc_reserve(size_t count, size_t align_pages, uintptr_t* out_base);
bool vaddr_alloc_release(uintptr_t base, size_t count);

size_t vaddr_alloc_total_page_count(void);
size_t vaddr_alloc_free_page_count(void);
