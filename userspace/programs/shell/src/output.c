#include <libc/string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "shell.h"

bool shell_write(const void* data, size_t size) {
	const uint8_t* bytes  = data;
	size_t         offset = 0u;

	if (data == NULL && size != 0u) return false;
	while (offset < size) {
		ssize_t written = write(1, bytes + offset, size - offset);
		if (written <= 0 || (size_t)written > size - offset) return false;
		offset += (size_t)written;
	}
	return true;
}

bool shell_write_string(const char* string) {
	return string != NULL && shell_write(string, strlen(string));
}

bool shell_write_prompt(const struct shell* shell) {
	return shell != NULL && shell_write_string("\x1b[1;34mkernel\x1b[0m:") && shell_write_string("\x1b[1;36m") &&
	       shell_write(shell->current_directory, shell->current_directory_size) &&
	       shell_write_string("\x1b[0m\x1b[1;32m$\x1b[0m ");
}

bool shell_write_command_error(const char* command, const char* message) {
	return shell_write_string("\x1b[1;31m") && shell_write_string(command) && shell_write_string(":\x1b[0m ") &&
	       shell_write_string(message) && shell_write_string("\n");
}

bool shell_write_unknown_command(const char* command, size_t size) {
	return shell_write_string("\x1b[1;31mshell:\x1b[0m command not found: ") && shell_write(command, size) &&
	       shell_write_string("\n");
}
