#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shell.h"

#define SHELL_ERROR_PREFIX "\x1b[1;31mshell:\x1b[0m "

static bool write_error(const char* command, const char* message) {
	if (message == NULL) return false;
	if (command == NULL) return printf(SHELL_ERROR_PREFIX "%s\n", message) >= 0;
	return printf(SHELL_ERROR_PREFIX "%s: %s\n", command, message) >= 0;
}

static bool write_unknown_command(const char* command) {
	return command != NULL && printf(SHELL_ERROR_PREFIX "command not found: %s\n", command) >= 0;
}

static bool write_program_exit(const char* command, uintptr_t exit_code) {
	return command != NULL && printf(SHELL_ERROR_PREFIX "%s: exited with code %llu (0x%llx)\n",
	                                 command,
	                                 (unsigned long long)exit_code,
	                                 (unsigned long long)exit_code) >= 0;
}

static bool is_space(char value) {
	return value == ' ' || value == '\t';
}

enum word_parse_result {
	WORD_PARSE_OK,
	WORD_PARSE_UNTERMINATED_QUOTE,
	WORD_PARSE_TRAILING_ESCAPE,
	WORD_PARSE_FAILED,
};

static bool report_change_directory(enum shell_change_directory_result result) {
	switch (result) {
	case SHELL_CHANGE_DIRECTORY_OK:
		return true;
	case SHELL_CHANGE_DIRECTORY_INVALID_PATH:
		return write_error("cd", "invalid path");
	case SHELL_CHANGE_DIRECTORY_NOT_FOUND:
		return write_error("cd", "directory not found");
	case SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY:
		return write_error("cd", "not a directory");
	case SHELL_CHANGE_DIRECTORY_DENIED:
		return write_error("cd", "permission denied");
	case SHELL_CHANGE_DIRECTORY_FAILED:
	default:
		return write_error("cd", "filesystem operation failed");
	}
}

static bool execute_change_directory(struct shell* shell, size_t argc, char* const argv[]) {
	if (argc == 0u) return write_error("cd", "missing directory");
	if (argc != 1u) return write_error("cd", "expected one directory");
	return report_change_directory(shell_change_directory(shell, argv[0], strlen(argv[0])));
}

static bool execute_print_directory(const struct shell* shell, size_t argc, char* const argv[]) {
	/* Paths are kept lexically normalized and there are no symbolic links, so -L and -P are equivalent. */
	for (size_t i = 0u; i < argc; i++) {
		if (strcmp(argv[i], "-L") != 0 && strcmp(argv[i], "-P") != 0) return write_error("pwd", "unexpected argument");
	}
	return shell->current_directory_size <= INT32_MAX &&
	       printf("%.*s\n", (int)shell->current_directory_size, shell->current_directory) >= 0;
}

static bool execute_program(struct shell* shell, size_t argc, char* const argv[]) {
	size_t    command_size = strlen(argv[0]);
	uintptr_t exit_code;
	switch (shell_run_program(shell, argv[0], command_size, argc - 1u, (const char* const*)(argv + 1u), &exit_code)) {
	case SHELL_PROGRAM_OK:
		return true;
	case SHELL_PROGRAM_NOT_FOUND:
		return write_unknown_command(argv[0]);
	case SHELL_PROGRAM_EXIT_FAILED:
		return write_program_exit(argv[0], exit_code);
	case SHELL_PROGRAM_FAILED:
	default:
		return write_error(argv[0], "unable to launch program");
	}
}

static enum word_parse_result parse_words(const char* line, size_t size, char*** out_argv, size_t* out_argc) {
	char*  storage;
	char** argv;
	size_t argc = 0u;
	size_t input;
	size_t output;
	char   quote   = '\0';
	bool   in_word = false;

	if (line == NULL || size == 0u || out_argv == NULL || out_argc == NULL) return WORD_PARSE_FAILED;
	storage = malloc(size + 1u);
	argv    = malloc((size / 2u + 1u) * sizeof(*argv));
	if (storage == NULL || argv == NULL) {
		free(argv);
		free(storage);
		return WORD_PARSE_FAILED;
	}
	input  = 0u;
	output = 0u;
	while (input < size) {
		char value = line[input++];
		if (value == '\\') {
			if (input == size) {
				free(argv);
				free(storage);
				return WORD_PARSE_TRAILING_ESCAPE;
			}
			if (!in_word) {
				argv[argc++] = storage + output;
				in_word      = true;
			}
			storage[output++] = line[input++];
			continue;
		}
		if (quote != '\0') {
			if (value == quote) quote = '\0';
			else storage[output++] = value;
			continue;
		}
		if (value == '\'' || value == '"') {
			if (!in_word) {
				argv[argc++] = storage + output;
				in_word      = true;
			}
			quote = value;
		}
		else if (is_space(value)) {
			if (in_word) {
				storage[output++] = '\0';
				in_word           = false;
			}
		}
		else {
			if (!in_word) {
				argv[argc++] = storage + output;
				in_word      = true;
			}
			storage[output++] = value;
		}
	}
	if (quote != '\0') {
		free(argv);
		free(storage);
		return WORD_PARSE_UNTERMINATED_QUOTE;
	}
	if (in_word) storage[output++] = '\0';
	*out_argv = argv;
	*out_argc = argc;
	return WORD_PARSE_OK;
}

static void free_words(char** argv) {
	if (argv == NULL) return;
	free(argv[0]);
	free(argv);
}

bool shell_execute_line(struct shell* shell, const char* line, size_t size) {
	char**                 argv = NULL;
	size_t                 argc;
	size_t                 start = 0u;
	bool                   success;
	enum word_parse_result parse_result;

	if (shell == NULL || (line == NULL && size != 0u)) return false;
	while (start < size && is_space(line[start])) start++;
	if (start == size) return true;
	line += start;
	size -= start;
	parse_result = parse_words(line, size, &argv, &argc);
	if (parse_result == WORD_PARSE_UNTERMINATED_QUOTE) return write_error(NULL, "unterminated quoted string");
	if (parse_result == WORD_PARSE_TRAILING_ESCAPE) return write_error(NULL, "trailing escape character");
	if (parse_result != WORD_PARSE_OK || argc == 0u) return false;
	if (strcmp(argv[0], "cd") == 0) success = execute_change_directory(shell, argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "pwd") == 0) success = execute_print_directory(shell, argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "clear") == 0) {
		if (argc != 1u) success = write_error("clear", "expected no arguments");
		else success = printf("\x1b[2J\x1b[H") >= 0;
	}
	else success = execute_program(shell, argc, argv);
	free_words(argv);
	return success;
}
