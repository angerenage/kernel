#pragma once

#include <stdint.h>

enum {
	USER_UPCALL_ARGUMENT_COUNT = 5u,
};

typedef void user_upcall_entry_t(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4);
