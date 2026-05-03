#pragma once

#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum address_transfer_result {
	ADDRESS_TRANSFER_OK = 0,
	ADDRESS_TRANSFER_INVALID_ARGUMENTS,
	ADDRESS_TRANSFER_ADDRESS_OVERFLOW,
	ADDRESS_TRANSFER_NOT_MAPPED,
	ADDRESS_TRANSFER_NOT_USER,
	ADDRESS_TRANSFER_ACCESS_DENIED,
	ADDRESS_TRANSFER_FAULT_FAILED,
};

enum address_transfer_access {
	ADDRESS_TRANSFER_READ     = 1u << 0,
	ADDRESS_TRANSFER_WRITE    = 1u << 1,
	ADDRESS_TRANSFER_EXEC     = 1u << 2,
	ADDRESS_TRANSFER_USER     = 1u << 3,
	ADDRESS_TRANSFER_PRESENT  = 1u << 4,
	ADDRESS_TRANSFER_FAULT_IN = 1u << 5,
};

bool address_transfer_result_is_success(enum address_transfer_result result);

enum address_transfer_result address_space_validate_range(struct address_space* space, uintptr_t addr, size_t size,
                                                          uint32_t access);

enum address_transfer_result address_space_copy_from(struct address_space* src_space, uintptr_t src_addr, void* dst,
                                                     size_t size);
enum address_transfer_result address_space_copy_to(struct address_space* dst_space, uintptr_t dst_addr, const void* src,
                                                   size_t size);
enum address_transfer_result address_space_copy_between(struct address_space* dst_space, uintptr_t dst_addr,
                                                        struct address_space* src_space, uintptr_t src_addr,
                                                        size_t size);
