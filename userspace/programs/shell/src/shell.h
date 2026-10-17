#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SHELL_LINE_CAPACITY          = 4096u,
	SHELL_PATH_CAPACITY          = 4096u,
	SHELL_HISTORY_CAPACITY       = 64u,
	SHELL_PENDING_INPUT_CAPACITY = 4096u,
};

struct shell_history_entry {
	char*  line;
	size_t size;
};

/* Mutable state owned by one shell process. */
struct shell {
	cap_id_t                   self_cap;
	process_id_t               pid;
	cap_id_t                   vfs_cap;
	cap_id_t                   current_directory_cap;
	cap_rights_t               current_directory_rights;
	char                       current_directory[SHELL_PATH_CAPACITY];
	size_t                     current_directory_size;
	size_t                     terminal_columns;
	size_t                     terminal_rows;
	uint8_t                    pending_input[SHELL_PENDING_INPUT_CAPACITY];
	size_t                     pending_input_head;
	size_t                     pending_input_count;
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
	SHELL_CHANGE_DIRECTORY_LOOP,
	SHELL_CHANGE_DIRECTORY_FAILED,
};

enum shell_program_result {
	SHELL_PROGRAM_OK,
	SHELL_PROGRAM_NOT_FOUND,
	SHELL_PROGRAM_EXIT_FAILED,
	SHELL_PROGRAM_FAILED,
};

/* Acquire the VFS service and initialize the shell in the root directory. */
bool shell_initialize(struct shell* shell);

/* Release resources retained by the shell. */
void shell_destroy(struct shell* shell);

/* Resolve and enter an absolute or shell-relative directory, following every symbolic link when physical. */
enum shell_change_directory_result shell_change_directory(struct shell* shell, const char* path, size_t path_size,
                                                          bool physical);

/* Normalize an absolute or shell-relative path without accessing it. */
bool shell_resolve_path(const struct shell* shell, const char* path, size_t path_size, char* output, size_t* out_size);

/* Load, run, and wait for one foreground program. */
enum shell_program_result shell_run_program(struct shell* shell, const char* command, size_t command_size, size_t argc,
                                            const char* const argv[], uintptr_t* out_exit_code);

/* Read and locally edit one line, including navigation through shell history. */
enum shell_input_result shell_read_line(struct shell* shell, char* line, size_t capacity, size_t* out_size);

/* Execute one submitted command line. */
bool shell_execute_line(struct shell* shell, const char* line, size_t size);

/* Return the displayed prompt width in fixed terminal cells. */
size_t shell_prompt_width(const struct shell* shell);

/* Display the colored prompt for the shell's current directory. */
bool shell_write_prompt(const struct shell* shell);
