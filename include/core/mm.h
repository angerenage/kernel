#pragma once

#include <stdint.h>

#define MM_KERNEL_ADDRESS_SPACE_BASE 0xffffffffa0000000ull
#define MM_KERNEL_ADDRESS_SPACE_SIZE 0x40000000ull
#define MM_USER_ADDRESS_SPACE_SIZE 0x40000000ull

/* Global address-space facts supplied by the kernel during memory initialization. */
struct mm_info {
	uintptr_t direct_map_offset;
};

extern struct mm_info mm_info;
