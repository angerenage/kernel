#pragma once

#include <core/kheap.h>
#include <criterion/criterion.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

void init_test_kheap(uint8_t* arena, size_t arena_size);
bool is_kheap_aligned(const void* ptr);
