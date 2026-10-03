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

enum runtime_startup_result runtime_startup_unpack(const struct process_startup_info* startup,
                                                   struct runtime_startup_arguments*  out_arguments) {
	char** argv;
	char*  cursor;
	char*  end;
	size_t capability_size;
	size_t payload_offset;

	if (out_arguments == NULL) return RUNTIME_STARTUP_INVALID;
	*out_arguments = (struct runtime_startup_arguments){0};

	if (startup == NULL || startup->size < sizeof(*startup) || startup->argc > INT_MAX) {
		return RUNTIME_STARTUP_INVALID;
	}
	capability_size = (size_t)startup->capc * sizeof(cap_id_t);
	payload_offset  = sizeof(*startup) + capability_size;
	if ((startup->capc == 0u && startup->capv_offset != 0u) ||
	    (startup->capc != 0u && startup->capv_offset != sizeof(*startup)) || payload_offset > startup->size) {
		return RUNTIME_STARTUP_INVALID;
	}
	out_arguments->capc = startup->capc;
	out_arguments->capv =
		startup->capc == 0u ? NULL : (const cap_id_t*)((const uint8_t*)startup + startup->capv_offset);
	if (startup->argc == 0u) {
		if (startup->argv_offset != 0u || startup->argv_size != 0u || payload_offset != startup->size)
			return RUNTIME_STARTUP_INVALID;
		return RUNTIME_STARTUP_OK;
	}
	if (startup->argv_offset != payload_offset || startup->argv_size == 0u ||
	    startup->argv_size > startup->size - startup->argv_offset ||
	    startup->size != startup->argv_offset + startup->argv_size || startup->argc > startup->argv_size ||
	    (size_t)startup->argc + 1u > SIZE_MAX / sizeof(*argv)) {
		return RUNTIME_STARTUP_INVALID;
	}

	argv = malloc(((size_t)startup->argc + 1u) * sizeof(*argv));
	if (argv == NULL) return RUNTIME_STARTUP_NO_MEMORY;

	cursor = (char*)startup + startup->argv_offset;
	end    = cursor + startup->argv_size;
	for (uint32_t i = 0u; i < startup->argc; i++) {
		char* terminator;
		if (cursor >= end || (terminator = bounded_nul(cursor, end)) == NULL) {
			free(argv);
			return RUNTIME_STARTUP_INVALID;
		}
		argv[i] = cursor;
		cursor  = terminator + 1;
	}
	if (cursor != end) {
		free(argv);
		return RUNTIME_STARTUP_INVALID;
	}

	argv[startup->argc] = NULL;
	out_arguments->argc = (int)startup->argc;
	out_arguments->argv = argv;
	return RUNTIME_STARTUP_OK;
}

void runtime_startup_arguments_deinit(struct runtime_startup_arguments* arguments) {
	if (arguments == NULL) return;
	free(arguments->argv);
	*arguments = (struct runtime_startup_arguments){0};
}
