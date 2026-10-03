#pragma once

#include <base/startup.h>

/* Result of rebuilding a program's positional startup arguments. */
enum runtime_startup_result {
	RUNTIME_STARTUP_OK = 0,
	RUNTIME_STARTUP_INVALID,
	RUNTIME_STARTUP_NO_MEMORY,
};

struct runtime_startup_arguments {
	int             argc;
	char**          argv;
	size_t          capc;
	const cap_id_t* capv;
};

/* Rebuild argv and expose the ordered capability list from one startup payload. */
enum runtime_startup_result runtime_startup_unpack(const struct process_startup_info* startup,
                                                   struct runtime_startup_arguments*  out_arguments);

/* Release allocations owned by runtime_startup_arguments. */
void runtime_startup_arguments_deinit(struct runtime_startup_arguments* arguments);
