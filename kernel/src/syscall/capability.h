#pragma once

#include <base/syscall.h>

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
syscall_result_t syscall_cap_valid(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cap_drop(uintptr_t arg0, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
syscall_result_t syscall_cap_unpublish(uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
