#pragma once

#include <core/early_alloc.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

#define PMM_TEST_LOW_OFFSET KiB(0)
#define PMM_TEST_LOW_LENGTH KiB(24)
#define PMM_TEST_HIGH_OFFSET KiB(64)
#define PMM_TEST_HIGH_LENGTH KiB(40)

void init_test_pmm(uint8_t* arena, size_t arena_size);
bool phys_in_range(uintptr_t phys, uintptr_t base, size_t length);
