#pragma once

#include <core/memory_backing.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>

#define BACKING_TEST_ARENA_SIZE (1024u * 1024u)

extern _Alignas(65536) uint8_t backing_test_arena[BACKING_TEST_ARENA_SIZE];

void   backing_test_pmm_init(void);
size_t backing_test_granule(void);
