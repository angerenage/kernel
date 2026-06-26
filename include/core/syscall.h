#pragma once

#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Common entry-point shape implemented by every syscall handler. */
typedef syscall_result_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

/* Get the current user address space for syscall operations. */
struct address_space* syscall_current_user_space(void);

/* Copy a string argument from user space to kernel space. */
syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string);

/* Write a uintptr_t value to user space. */
syscall_result_t syscall_write_uintptr_arg(struct address_space* space, uintptr_t dst, uintptr_t arg_index,
                                           uintptr_t value);

/* Copy data to user space. */
syscall_result_t syscall_copy_to_user(struct address_space* space, uintptr_t dst, const void* src, size_t size,
                                      uintptr_t arg_index);

syscall_result_t syscall_nop(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_yield(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_tick_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_exit_process(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_exit_thread(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_spawn_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5);
syscall_result_t syscall_join_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_detach_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cancel_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_set_thread_cancel_enabled(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                                   uintptr_t);
syscall_result_t syscall_test_thread_cancel(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_send_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_recv_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_cap_create(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t);
syscall_result_t syscall_cap_delegate(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t,
                                      uintptr_t);
syscall_result_t syscall_cap_derive(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t);
syscall_result_t syscall_cap_call(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cap_revoke(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cap_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t, uintptr_t);
