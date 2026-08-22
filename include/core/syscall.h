#pragma once

#include <base/syscall.h>
#include <core/address_transfer.h>
#include <stddef.h>
#include <stdint.h>

/* Get the current user address space for syscall operations. */
struct address_space* syscall_current_user_space(void);

/* Convert an address-transfer validation or copy result into the public syscall ABI. */
syscall_result_t syscall_result_from_address_transfer(enum address_transfer_result result, uintptr_t arg_index);

/* Copy a string argument from user space to kernel space. */
syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string);

/* Write a uintptr_t value to user space. */
syscall_result_t syscall_write_uintptr_arg(struct address_space* space, uintptr_t dst, uintptr_t arg_index,
                                           uintptr_t value);

/* Copy data to user space. */
syscall_result_t syscall_copy_to_user(struct address_space* space, uintptr_t dst, const void* src, size_t size,
                                      uintptr_t arg_index);

/* Copy data from user space. */
syscall_result_t syscall_copy_from_user(struct address_space* space, uintptr_t src, void* dst, size_t size,
                                        uintptr_t arg_index);
