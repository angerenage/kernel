#pragma once

#include <core/early_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool is_aligned_uintptr(uintptr_t p, size_t align);
void init_early_allocator(uint8_t* arena, size_t arena_size);
