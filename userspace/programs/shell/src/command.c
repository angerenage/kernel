#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>

#include "shell.h"

static bool is_space(char value) {
	return value == ' ' || value == '\t';
}

static bool report_change_directory(enum shell_change_directory_result result) {
	switch (result) {
	case SHELL_CHANGE_DIRECTORY_OK:
		return true;
	case SHELL_CHANGE_DIRECTORY_INVALID_PATH:
		return shell_write_command_error("cd", "invalid path");
	case SHELL_CHANGE_DIRECTORY_NOT_FOUND:
		return shell_write_command_error("cd", "directory not found");
	case SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY:
		return shell_write_command_error("cd", "not a directory");
	case SHELL_CHANGE_DIRECTORY_DENIED:
		return shell_write_command_error("cd", "permission denied");
	case SHELL_CHANGE_DIRECTORY_FAILED:
	default:
		return shell_write_command_error("cd", "filesystem operation failed");
	}
}

static bool execute_change_directory(struct shell* shell, size_t argc, char* const argv[]) {
	if (argc == 0u) return shell_write_command_error("cd", "missing directory");
	if (argc != 1u) return shell_write_command_error("cd", "expected one directory");
	return report_change_directory(shell_change_directory(shell, argv[0], strlen(argv[0])));
}

static bool execute_program(struct shell* shell, size_t argc, char* const argv[]) {
	size_t command_size = strlen(argv[0]);
	switch (shell_run_program(shell, argv[0], command_size, argc - 1u, (const char* const*)(argv + 1u))) {
	case SHELL_PROGRAM_OK:
		return true;
	case SHELL_PROGRAM_NOT_FOUND:
		return shell_write_unknown_command(argv[0], command_size);
	case SHELL_PROGRAM_EXIT_FAILED:
		return shell_write_command_error(argv[0], "program exited unsuccessfully");
	case SHELL_PROGRAM_FAILED:
	default:
		return shell_write_command_error(argv[0], "unable to launch program");
	}
}

static bool parse_words(const char* line, size_t size, char*** out_argv, size_t* out_argc) {
	char*  storage;
	char** argv;
	size_t argc = 0u;
	size_t offset;

	if (line == NULL || size == 0u || out_argv == NULL || out_argc == NULL) return false;
	storage = malloc(size + 1u);
	argv    = malloc((size / 2u + 1u) * sizeof(*argv));
	if (storage == NULL || argv == NULL) {
		free(argv);
		free(storage);
		return false;
	}
	memcpy(storage, line, size);
	storage[size] = '\0';
	offset        = 0u;
	while (offset < size) {
		while (offset < size && is_space(storage[offset])) storage[offset++] = '\0';
		if (offset == size) break;
		argv[argc++] = storage + offset;
		while (offset < size && !is_space(storage[offset])) offset++;
	}
	*out_argv = argv;
	*out_argc = argc;
	return true;
}

static void free_words(char** argv) {
	if (argv == NULL) return;
	free(argv[0]);
	free(argv);
}

bool shell_execute_line(struct shell* shell, const char* line, size_t size) {
	char** argv = NULL;
	size_t argc;
	size_t start = 0u;
	size_t end   = size;
	bool   success;

	if (shell == NULL || (line == NULL && size != 0u)) return false;
	while (start < size && is_space(line[start])) start++;
	while (end > start && is_space(line[end - 1u])) end--;
	if (start == end) return true;
	line += start;
	size = end - start;
	if (!parse_words(line, size, &argv, &argc) || argc == 0u) return false;
	if (strcmp(argv[0], "cd") == 0) success = execute_change_directory(shell, argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "clear") == 0) {
		if (argc != 1u) success = shell_write_command_error("clear", "expected no arguments");
		else success = shell_write_string("\x1b[2J\x1b[H");
	}
	else success = execute_program(shell, argc, argv);
	free_words(argv);
	return success;
}
