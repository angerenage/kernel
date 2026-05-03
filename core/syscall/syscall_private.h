#pragma once

#include <core/syscall.h>
#include <stdint.h>

syscall_result_t syscall_nop(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_yield(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_sleep_ms(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_tick_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_getpid(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_exit_process(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_create_process(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_run_process(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t,
                                     uintptr_t);
syscall_result_t syscall_wait_process(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_detach_process(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_kill_process(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_get_process_thread_count(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_exit_thread(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_spawn_thread(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t arg5);
syscall_result_t syscall_join_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_detach_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cancel_thread(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_set_thread_cancel_enabled(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                                   uintptr_t);
syscall_result_t syscall_test_thread_cancel(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_gettid(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string);
