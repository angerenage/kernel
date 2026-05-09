#pragma once

#include <base/vmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stdint.h>

bool page_table_map(struct hal_address_space* table, uintptr_t virt, uintptr_t phys, vmm_prot_t prot);
bool page_table_unmap(struct hal_address_space* table, uintptr_t virt);
bool page_table_query(const struct hal_address_space* table, uintptr_t virt, uintptr_t* out_phys, uint64_t* out_flags);
bool page_table_remap_prot(struct hal_address_space* table, uintptr_t virt, uintptr_t phys, vmm_prot_t prot);
