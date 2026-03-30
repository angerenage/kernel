#pragma once

#include <core/vaddr_alloc.h>
#include <criterion/criterion.h>
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

void init_test_vaddr_alloc(uint8_t* arena, size_t arena_size, uintptr_t base, size_t page_count);
