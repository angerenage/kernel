#pragma once

#include <core/syscall.h>
#include <stddef.h>
#include <stdint.h>

syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5);

static inline syscall_result_t print(const char* data, size_t length) {
	return syscall(SYSCALL_PRINT, (uintptr_t)data, (uintptr_t)length, 0u, 0u, 0u, 0u);
}

static inline syscall_result_t exit_process(uintptr_t code) {
	return syscall(SYSCALL_EXIT_PROCESS, code, 0u, 0u, 0u, 0u, 0u);
}

static inline syscall_result_t exit_thread(uintptr_t code) {
	return syscall(SYSCALL_EXIT_THREAD, code, 0u, 0u, 0u, 0u, 0u);
}
