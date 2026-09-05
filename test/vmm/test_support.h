#pragma once

#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/thread.h>
#include <core/vm_space.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KiB(x) ((size_t)(x) * 1024u)

void   init_test_vmm(uint8_t* arena, size_t arena_size);
size_t vmm_test_bytes_consumed_since(size_t free_before);
bool   test_vm_map(struct address_space* space, size_t page_count, vmm_prot_t prot, uintptr_t requested_base,
                   size_t align_pages, size_t guard_pages, vmm_id_t* out_id, void** out_base);
void   mock_paging_reset(void);
void   mock_paging_fail_init_once(void);
void   mock_paging_fail_after(size_t successful_maps);
void   mock_paging_fail_once_after(size_t successful_maps);
void   mock_paging_fail_next_unmap(void);
size_t mock_paging_mapping_count(void);
