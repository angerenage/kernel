#pragma once

#include <base/syscall.h>
#include <stdint.h>

/* Common entry-point shape implemented by every syscall handler. */
typedef syscall_result_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

/* Copy a string argument from user space to kernel space. */
syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string);
