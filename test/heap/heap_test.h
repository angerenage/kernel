#pragma once

#include <base/heap.h>
#include <criterion/criterion.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

void init_test_heap(uint8_t* arena, size_t arena_size);
bool is_heap_aligned(const void* ptr);
