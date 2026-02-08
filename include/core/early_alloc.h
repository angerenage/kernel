#pragma once

#include <core/mm.h>
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool early_init(const struct limine_memmap_response* memmap_resp, uintptr_t direct_map_offset);

void* early_alloc(size_t size, size_t align);
void* early_calloc(size_t nmemb, size_t size, size_t align);

uint64_t early_remaining_bytes(void);
