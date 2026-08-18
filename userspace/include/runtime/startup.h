#pragma once

#include <base/startup.h>

/* Result of rebuilding the conventional argv pointer vector from startup data. */
enum runtime_startup_argv_result {
	RUNTIME_STARTUP_ARGV_OK = 0,
	RUNTIME_STARTUP_ARGV_INVALID,
	RUNTIME_STARTUP_ARGV_NO_MEMORY,
};

/* Rebuild argv from the serialized startup payload. Runtime heap must already be initialized. argv[argc] is NULL. */
enum runtime_startup_argv_result runtime_startup_unpack_argv(const struct process_startup_info* startup, int* out_argc,
                                                             char*** out_argv);

/* Release the pointer vector returned by runtime_startup_unpack_argv(). */
void runtime_startup_free_argv(char** argv);
