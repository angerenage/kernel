#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>

#include "shell.h"

static bool is_space(char value) {
	return value == ' ' || value == '\t';
}

static bool command_is(const char* line, size_t size, const char* command) {
	size_t command_size = strlen(command);
	return size == command_size && memcmp(line, command, command_size) == 0;
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

static bool execute_change_directory(struct shell* shell, const char* arguments, size_t size) {
	size_t path_start = 0u;
	size_t path_end   = size;

	while (path_start < size && is_space(arguments[path_start])) path_start++;
	while (path_end > path_start && is_space(arguments[path_end - 1u])) path_end--;
	if (path_start == path_end) return shell_write_command_error("cd", "missing directory");
	for (size_t i = path_start; i < path_end; i++) {
		if (is_space(arguments[i])) return shell_write_command_error("cd", "expected one directory");
	}
	return report_change_directory(shell_change_directory(shell, arguments + path_start, path_end - path_start));
}

bool shell_execute_line(struct shell* shell, const char* line, size_t size) {
	size_t start       = 0u;
	size_t end         = size;
	size_t command_end = 0u;

	if (shell == NULL || (line == NULL && size != 0u)) return false;
	while (start < size && is_space(line[start])) start++;
	while (end > start && is_space(line[end - 1u])) end--;
	if (start == end) return true;
	line += start;
	size = end - start;
	while (command_end < size && !is_space(line[command_end])) command_end++;
	if (command_is(line, command_end, "cd"))
		return execute_change_directory(shell, line + command_end, size - command_end);
	if (command_is(line, command_end, "clear")) {
		if (command_end != size) return shell_write_command_error("clear", "expected no arguments");
		return shell_write_string("\x1b[2J\x1b[H");
	}
	return shell_write_unknown_command(line, command_end);
}
