#pragma once

#include <core/vmm.h>
#include <criterion/criterion.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)

void   init_test_vmm(uint8_t* arena, size_t arena_size);
void   mock_paging_reset(void);
void   mock_paging_fail_after(size_t successful_maps);
size_t mock_paging_mapping_count(void);
