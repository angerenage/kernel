#include <base/process.h>
#include <base/startup.h>
#include <libc/string.h>
#include <protocol/filesystem.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/process.h>

#include "foreground_input.h"
#include "shell.h"

#define SHELL_LOADER_SERVICE "elf64"
#define SHELL_PROGRAM_DIRECTORY "/boot/"
#define SHELL_PROGRAM_SUFFIX ".elf"

static bool command_has_suffix(const char* command, size_t size) {
	const size_t suffix_size = sizeof(SHELL_PROGRAM_SUFFIX) - 1u;
	return size >= suffix_size && memcmp(command + size - suffix_size, SHELL_PROGRAM_SUFFIX, suffix_size) == 0;
}

static bool executable_path(const struct shell* shell, const char* command, size_t command_size, char* output,
                            size_t* out_size) {
	if (memchr(command, '/', command_size) != NULL)
		return shell_resolve_path(shell, command, command_size, output, out_size);
	const size_t directory_size = sizeof(SHELL_PROGRAM_DIRECTORY) - 1u;
	const size_t suffix_size    = command_has_suffix(command, command_size) ? 0u : sizeof(SHELL_PROGRAM_SUFFIX) - 1u;
	if (command_size == 0u || command_size > FILESYSTEM_NAME_MAX - suffix_size ||
	    directory_size + command_size + suffix_size > SHELL_PATH_CAPACITY)
		return false;
	memcpy(output, SHELL_PROGRAM_DIRECTORY, directory_size);
	memcpy(output + directory_size, command, command_size);
	if (suffix_size != 0u) memcpy(output + directory_size + command_size, SHELL_PROGRAM_SUFFIX, suffix_size);
	*out_size = directory_size + command_size + suffix_size;
	return true;
}

static void process_name(const char* command, size_t command_size, const char** out_name, size_t* out_size) {
	size_t offset = command_size;
	while (offset != 0u && command[offset - 1u] != '/') offset--;
	*out_name = command + offset;
	*out_size = command_size - offset;
}

static void stop_process(struct program_run_result* running) {
	if (running->thread_cap != CAP_ID_INVALID) {
		(void)cap_drop(running->thread_cap);
		running->thread_cap = CAP_ID_INVALID;
	}
	if (running->process_cap != CAP_ID_INVALID) {
		(void)process_kill(running->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		if (process_wait(running->process_cap, NULL) != SYSCALL_STATUS_OK) (void)cap_drop(running->process_cap);
		running->process_cap = CAP_ID_INVALID;
	}
}

enum shell_program_result shell_run_program(struct shell* shell, const char* command, size_t command_size, size_t argc,
                                            const char* const argv[], uintptr_t* out_exit_code) {
	struct filesystem_node_handle executable = {.capability = CAP_ID_INVALID};
	struct program_load_result    loaded     = {
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};
	struct program_run_result running = {
		.process_cap = CAP_ID_INVALID,
		.thread_cap  = CAP_ID_INVALID,
	};
	struct program_capability_argument capabilities[PROCESS_STARTUP_CAP_COUNT] = {0};
	struct filesystem_call_result      opened;
	char                               path[SHELL_PATH_CAPACITY];
	const char*                        name;
	size_t                             name_size;
	size_t                             path_size;
	uintptr_t                          exit_code        = PROCESS_EXIT_SYSTEM_UNKNOWN;
	struct shell_foreground_input*     foreground_input = NULL;
	syscall_status_t                   status;
	enum shell_program_result          result = SHELL_PROGRAM_FAILED;

	if (out_exit_code != NULL) *out_exit_code = PROCESS_EXIT_SYSTEM_UNKNOWN;
	if (shell == NULL || shell->vfs_cap == CAP_ID_INVALID || command == NULL || command_size == 0u ||
	    (argc != 0u && argv == NULL) || !executable_path(shell, command, command_size, path, &path_size))
		return SHELL_PROGRAM_FAILED;
	opened = vfs_open(shell->vfs_cap, path, path_size, CAP_READ | CAP_DELEGATE, &executable);
	if (opened.transport_status == SYSCALL_STATUS_OK && opened.status == FILESYSTEM_STATUS_NOT_FOUND) {
		if (executable.capability != CAP_ID_INVALID) (void)cap_drop(executable.capability);
		return SHELL_PROGRAM_NOT_FOUND;
	}
	if (opened.transport_status != SYSCALL_STATUS_OK || opened.status != FILESYSTEM_STATUS_OK ||
	    executable.info.type != FILESYSTEM_NODE_FILE || executable.capability == CAP_ID_INVALID)
		goto cleanup;
	process_name(command, command_size, &name, &name_size);
	if (name_size == 0u) goto cleanup;
	status = program_load(SHELL_LOADER_SERVICE, executable.capability, name, name_size, &loaded);
	if (cap_drop(executable.capability) != SYSCALL_STATUS_OK) {
		executable.capability = CAP_ID_INVALID;
		goto cleanup;
	}
	executable.capability = CAP_ID_INVALID;
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	foreground_input = shell_foreground_input_create(shell);
	if (foreground_input == NULL) goto cleanup;
	capabilities[PROCESS_STARTUP_CAP_STDIN].capability = shell_foreground_input_capability(foreground_input);
	capabilities[PROCESS_STARTUP_CAP_STDIN].rights     = CAP_CALL | CAP_READ;
	display_get_standard_streams(NULL,
	                             &capabilities[PROCESS_STARTUP_CAP_STDOUT].capability,
	                             &capabilities[PROCESS_STARTUP_CAP_STDERR].capability);
	capabilities[PROCESS_STARTUP_CAP_STDOUT].rights     = CAP_CALL | CAP_WRITE;
	capabilities[PROCESS_STARTUP_CAP_STDERR].rights     = CAP_CALL | CAP_WRITE;
	capabilities[PROCESS_STARTUP_CAP_WORKING_DIRECTORY] = (struct program_capability_argument){
		.capability = shell->current_directory_cap,
		.rights     = shell->current_directory_rights,
	};
	for (size_t i = 0u; i < PROCESS_STARTUP_CAP_COUNT; i++)
		if (capabilities[i].capability == CAP_ID_INVALID) capabilities[i].rights = 0u;
	status = program_run(&loaded, argc, argv, PROCESS_STARTUP_CAP_COUNT, capabilities, &running);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	loaded.load_cap = CAP_ID_INVALID;
	if (!shell_foreground_input_start(foreground_input, running.process_cap)) goto cleanup;
	status = cap_drop(running.thread_cap);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	running.thread_cap = CAP_ID_INVALID;
	status             = process_wait(running.process_cap, &exit_code);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	running.process_cap = CAP_ID_INVALID;
	if (out_exit_code != NULL) *out_exit_code = exit_code;
	result = exit_code == PROCESS_EXIT_SUCCESS ? SHELL_PROGRAM_OK : SHELL_PROGRAM_EXIT_FAILED;

cleanup:
	if (!shell_foreground_input_destroy(foreground_input)) result = SHELL_PROGRAM_FAILED;
	if (executable.capability != CAP_ID_INVALID) (void)cap_drop(executable.capability);
	if (loaded.load_cap != CAP_ID_INVALID) (void)program_cancel(loaded.load_cap);
	stop_process(&running);
	return result;
}
