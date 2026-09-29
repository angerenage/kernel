#pragma once

#include <boot/info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MM_KERNEL_ADDRESS_SPACE_BASE 0xffffffffa0000000ull
#define MM_KERNEL_ADDRESS_SPACE_SIZE 0x40000000ull
#define MM_USER_ADDRESS_SPACE_SIZE 0x40000000ull

/* Global boot address-space facts needed by allocators that translate physical memory through the direct map. */
struct mm_boot_info {
	uintptr_t direct_map_offset;
};

extern struct mm_boot_info boot_info;

/* Convert a mem_range_type value to a short diagnostic string. */
const char* mem_range_type_str(enum mem_range_type type);
