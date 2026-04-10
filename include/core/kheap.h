#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool kheap_init(void);
bool kheap_is_initialized(void);

void* kmalloc(size_t size);
void  kfree(void* ptr);
void* kcalloc(size_t nmemb, size_t size);
void* krealloc(void* ptr, size_t size);

bool kheap_grow_pages(size_t page_count, void** out_base);

size_t kheap_total_bytes(void);
size_t kheap_free_bytes(void);
