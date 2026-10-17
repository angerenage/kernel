#include "shell.h"

#include <libc/stdlib.h>
#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/init.h>
#include <runtime/terminal.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/process.h>

#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u

bool shell_initialize(struct shell* shell) {
	static const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle        service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle     root    = {.capability = CAP_ID_INVALID};
	struct init_call_result           acquired;
	struct filesystem_call_result     opened;
	struct terminal_get_info_response terminal_info;
	struct self_info                  self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	cap_id_t terminal_cap;

	if (shell == NULL) return false;
	*shell = (struct shell){.self_cap              = CAP_ID_INVALID,
	                        .pid                   = PROCESS_PID_INVALID,
	                        .vfs_cap               = CAP_ID_INVALID,
	                        .current_directory_cap = CAP_ID_INVALID};
	display_get_standard_streams(&terminal_cap, NULL, NULL);
	if (terminal_get_info(terminal_cap, &terminal_info) != SYSCALL_STATUS_OK || terminal_info.columns == 0u ||
	    terminal_info.columns > SIZE_MAX || terminal_info.rows == 0u || terminal_info.rows > INT64_MAX ||
	    terminal_info.rows > SIZE_MAX / terminal_info.columns)
		return false;
	shell->terminal_columns = (size_t)terminal_info.columns;
	shell->terminal_rows    = (size_t)terminal_info.rows;
	if (process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	shell->self_cap = self.self_cap;
	shell->pid      = self.pid;
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (shell->self_cap == CAP_ID_INVALID) return false;

	acquired = init_acquire(&query, VFS_SERVICE_NAME, &service);
	if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK ||
	    service.capability == CAP_ID_INVALID) {
		shell_destroy(shell);
		return false;
	}
	opened = vfs_open(service.capability, "/", 1u, CAP_READ | CAP_DELEGATE, &root);
	if (opened.transport_status != SYSCALL_STATUS_OK || opened.status != FILESYSTEM_STATUS_OK ||
	    root.info.type != FILESYSTEM_NODE_DIRECTORY || root.capability == CAP_ID_INVALID) {
		if (root.capability != CAP_ID_INVALID) (void)cap_drop(root.capability);
		(void)cap_drop(service.capability);
		shell_destroy(shell);
		return false;
	}

	shell->vfs_cap                = service.capability;
	shell->current_directory_cap  = root.capability;
	shell->current_directory[0]   = '/';
	shell->current_directory_size = 1u;
	return true;
}

void shell_destroy(struct shell* shell) {
	if (shell == NULL) return;
	for (size_t i = 0u; i < shell->history_count; i++) free(shell->history[i].line);
	shell->history_count = 0u;
	if (shell->current_directory_cap != CAP_ID_INVALID) (void)cap_drop(shell->current_directory_cap);
	shell->current_directory_cap = CAP_ID_INVALID;
	if (shell->vfs_cap != CAP_ID_INVALID) (void)cap_drop(shell->vfs_cap);
	shell->vfs_cap = CAP_ID_INVALID;
	if (shell->self_cap != CAP_ID_INVALID) (void)cap_drop(shell->self_cap);
	shell->self_cap = CAP_ID_INVALID;
	shell->pid      = PROCESS_PID_INVALID;
}

size_t shell_prompt_width(const struct shell* shell) {
	size_t width = 9u;

	if (shell == NULL) return 0u;
	for (size_t i = 0u; i < shell->current_directory_size; i++) {
		if (((uint8_t)shell->current_directory[i] & 0xc0u) != 0x80u) width++;
	}
	return width;
}

bool shell_write_prompt(const struct shell* shell) {
	if (shell == NULL || shell->terminal_columns == 0u || shell->current_directory_size > INT32_MAX ||
	    printf("\x1b[1;34mkernel\x1b[0m:\x1b[1;36m%.*s\x1b[0m\x1b[1;32m$\x1b[0m ",
	           (int)shell->current_directory_size,
	           shell->current_directory) < 0)
		return false;
	return shell_prompt_width(shell) % shell->terminal_columns != 0u || putchar('\n') != EOF;
}
