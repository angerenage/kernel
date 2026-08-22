#pragma once

#include <base/syscall.h>

struct hal_userspace_return_frame;

typedef syscall_result_t (*syscall_fn_t)(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                         uintptr_t arg5);

enum syscall_frame_action {
	SYSCALL_FRAME_WRITE_RESULT = 0,
	SYSCALL_FRAME_RESTORED,
};

/* Dispatch a syscall number through the kernel handler table. */
syscall_result_t syscall_dispatch(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                  uintptr_t arg4, uintptr_t arg5);

/* Dispatch one syscall using its native userspace frame. */
enum syscall_frame_action syscall_dispatch_user_frame(struct hal_userspace_return_frame* frame, uintptr_t number,
                                                      uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                                      uintptr_t arg4, uintptr_t arg5, syscall_result_t* out_result);
