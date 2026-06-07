#pragma once

#include <core/syscall.h>
#include <stdint.h>

syscall_result_t syscall_nop(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_print(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

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

syscall_result_t syscall_vm_reserve(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                    uintptr_t arg5);
syscall_result_t syscall_vm_reserve_at(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                       uintptr_t arg5);
syscall_result_t syscall_vm_free(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_vm_map(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_vm_unmap(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_vm_protect(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_vm_query(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_vm_copy_from(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t,
                                      uintptr_t);
syscall_result_t syscall_vm_copy_to(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t,
                                    uintptr_t);

syscall_result_t syscall_send_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_recv_message(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_channel_create(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_channel_send(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_channel_recv(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
                                      uintptr_t);
syscall_result_t syscall_channel_destroy(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_copy_out(uintptr_t ptr_arg_index, uintptr_t dst, const void* src, size_t size);

syscall_result_t syscall_module_resolve(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t, uintptr_t,
                                        uintptr_t);
syscall_result_t syscall_module_map(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

syscall_result_t syscall_copy_string_arg(uintptr_t ptr_arg_index, uintptr_t string_ptr, uintptr_t len_arg_index,
                                         uintptr_t string_len_arg, char** out_string);
