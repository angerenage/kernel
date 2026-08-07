#pragma once

#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

struct hal_userspace_return_frame;

/* Common entry-point shape implemented by every syscall handler. */
typedef syscall_result_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

/* Tell the architecture whether the dispatcher restored the frame. */
enum syscall_frame_action {
	SYSCALL_FRAME_WRITE_RESULT = 0,
	SYSCALL_FRAME_RESTORED,
};

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

/* Copy data from user space. */
syscall_result_t syscall_copy_from_user(struct address_space* space, uintptr_t src, void* dst, size_t size,
                                        uintptr_t arg_index);

/* Dispatch one syscall using its native userspace frame. */
enum syscall_frame_action syscall_dispatch_user_frame(struct hal_userspace_return_frame* frame, uintptr_t number,
                                                      uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                                      uintptr_t arg4, uintptr_t arg5, syscall_result_t* out_result);

syscall_result_t syscall_nop(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_yield(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_tick_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_exit_process(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_exit_thread(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

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
syscall_result_t syscall_cap_call(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                  uintptr_t);
syscall_result_t syscall_cap_reply(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t,
                                   uintptr_t);
syscall_result_t syscall_cap_revoke(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cap_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t, uintptr_t);

syscall_result_t syscall_upcall_dropped_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
