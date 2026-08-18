#include <libc/stdlib.h>
#include <limits.h>
#include <runtime/startup.h>
#include <stddef.h>
#include <stdint.h>

static char* bounded_nul(char* begin, char* end) {
	for (char* cursor = begin; cursor < end; cursor++) {
		if (*cursor == '\0') return cursor;
	}
	return NULL;
}

enum runtime_startup_argv_result runtime_startup_unpack_argv(const struct process_startup_info* startup, int* out_argc,
                                                             char*** out_argv) {
	char** argv;
	char*  cursor;
	char*  end;

	if (out_argc == NULL || out_argv == NULL) return RUNTIME_STARTUP_ARGV_INVALID;
	*out_argc = 0;
	*out_argv = NULL;

	if (startup == NULL || startup->size < sizeof(*startup) || startup->argc > INT_MAX) {
		return RUNTIME_STARTUP_ARGV_INVALID;
	}
	if (startup->argc == 0u) {
		return startup->argv_offset == 0u && startup->argv_size == 0u ? RUNTIME_STARTUP_ARGV_OK
		                                                              : RUNTIME_STARTUP_ARGV_INVALID;
	}
	if (startup->argv_offset < sizeof(*startup) || startup->argv_offset > startup->size || startup->argv_size == 0u ||
	    startup->argv_size > startup->size - startup->argv_offset || startup->argc > startup->argv_size ||
	    (size_t)startup->argc + 1u > SIZE_MAX / sizeof(*argv)) {
		return RUNTIME_STARTUP_ARGV_INVALID;
	}

	argv = malloc(((size_t)startup->argc + 1u) * sizeof(*argv));
	if (argv == NULL) return RUNTIME_STARTUP_ARGV_NO_MEMORY;

	cursor = (char*)startup + startup->argv_offset;
	end    = cursor + startup->argv_size;
	for (uint32_t i = 0u; i < startup->argc; i++) {
		char* terminator;
		if (cursor >= end || (terminator = bounded_nul(cursor, end)) == NULL) {
			free(argv);
			return RUNTIME_STARTUP_ARGV_INVALID;
		}
		argv[i] = cursor;
		cursor  = terminator + 1;
	}
	if (cursor != end) {
		free(argv);
		return RUNTIME_STARTUP_ARGV_INVALID;
	}

	argv[startup->argc] = NULL;
	*out_argc           = (int)startup->argc;
	*out_argv           = argv;
	return RUNTIME_STARTUP_ARGV_OK;
}

void runtime_startup_free_argv(char** argv) {
	free(argv);
}
