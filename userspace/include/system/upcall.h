#pragma once

/* Restore the context interrupted by the current upcall. */
__attribute__((noreturn))
void upcall_return(void);

/* Define a userspace upcall handler with the specified name and arguments. */
#define DEFINE_UPCALL_HANDLER(name, arg0, arg1, arg2, arg3, arg4)                                                      \
	static void name##_body(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4);           \
                                                                                                                       \
	static user_upcall_entry_t name __attribute__((noreturn));                                                         \
                                                                                                                       \
	static void name(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {                 \
		name##_body(arg0, arg1, arg2, arg3, arg4);                                                                     \
		upcall_return();                                                                                               \
	}                                                                                                                  \
                                                                                                                       \
	static void name##_body(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4)
