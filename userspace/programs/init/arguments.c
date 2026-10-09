#include "arguments.h"

#include <stdlib.h>
#include <string.h>

bool init_arguments_valid(uint32_t argc, const char* argv_data, size_t argv_size) {
	size_t offset = 0u;

	if (argc == 0u) return argv_data == NULL && argv_size == 0u;
	if (argv_data == NULL || argv_size == 0u || argc > argv_size) return false;
	for (uint32_t index = 0u; index < argc; index++) {
		const char* terminator = memchr(argv_data + offset, '\0', argv_size - offset);

		if (terminator == NULL) return false;
		offset = (size_t)(terminator - argv_data) + 1u;
	}
	return offset == argv_size;
}

bool init_arguments_vector(const struct init_state* init, const char*** out_argv) {
	const char** argv;
	const char*  cursor;

	if (out_argv == NULL) return false;
	*out_argv = NULL;
	if (init == NULL || !init_arguments_valid(init->argc, init->argv_data, init->argv_size)) return false;
	if (init->argc == 0u) return true;
	argv = malloc(init->argc * sizeof(*argv));
	if (argv == NULL) return false;
	cursor = init->argv_data;
	for (size_t index = 0u; index < init->argc; index++) {
		argv[index] = cursor;
		while (*cursor != '\0') cursor++;
		cursor++;
	}
	*out_argv = argv;
	return true;
}
