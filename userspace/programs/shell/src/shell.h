#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>

enum {
	SHELL_LINE_CAPACITY    = 4096u,
	SHELL_PATH_CAPACITY    = 4096u,
	SHELL_HISTORY_CAPACITY = 64u,
};

struct shell_history_entry {
	char*  line;
	size_t size;
};

/* Mutable state owned by one shell process. */
struct shell {
	cap_id_t                   vfs_cap;
	char                       current_directory[SHELL_PATH_CAPACITY];
	size_t                     current_directory_size;
	struct shell_history_entry history[SHELL_HISTORY_CAPACITY];
	size_t                     history_count;
};

enum shell_input_result {
	SHELL_INPUT_FAILED,
	SHELL_INPUT_SUBMITTED,
	SHELL_INPUT_INTERRUPTED,
};

enum shell_change_directory_result {
	SHELL_CHANGE_DIRECTORY_OK,
	SHELL_CHANGE_DIRECTORY_INVALID_PATH,
	SHELL_CHANGE_DIRECTORY_NOT_FOUND,
	SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY,
	SHELL_CHANGE_DIRECTORY_DENIED,
	SHELL_CHANGE_DIRECTORY_FAILED,
};

/* Acquire the VFS service and initialize the shell in the root directory. */
bool shell_initialize(struct shell* shell);

/* Release resources retained by the shell. */
bool shell_destroy(struct shell* shell);

/* Resolve and enter an absolute or shell-relative directory. */
enum shell_change_directory_result shell_change_directory(struct shell* shell, const char* path, size_t path_size);

/* Read and locally edit one line, including navigation through shell history. */
enum shell_input_result shell_read_line(struct shell* shell, char* line, size_t capacity, size_t* out_size);

/* Execute one submitted command line. */
bool shell_execute_line(struct shell* shell, const char* line, size_t size);

/* Write an exact byte sequence to standard output. */
bool shell_write(const void* data, size_t size);

/* Write a NUL-terminated string to standard output. */
bool shell_write_string(const char* string);

/* Display the colored prompt for the shell's current directory. */
bool shell_write_prompt(const struct shell* shell);

/* Display a diagnostic for a built-in command. */
bool shell_write_command_error(const char* command, const char* message);

/* Display a diagnostic for an unknown command name. */
bool shell_write_unknown_command(const char* command, size_t size);
