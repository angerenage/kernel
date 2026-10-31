#include "redirect.h"

#include <base/cap.h>
#include <libc/string.h>
#include <protocol/filesystem.h>
#include <runtime/filesystem.h>
#include <runtime/stream.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "parse.h"
#include "shell.h"

enum { REDIRECT_INPUT_ID = 1u, REDIRECT_OUTPUT_ID = 2u };

struct redirect_slot {
	cap_id_t file;
	cap_id_t stream;
	uint64_t offset;
	bool     active;
	bool     failed;
};

struct redirect_server {
	channel_id_t         endpoint;
	process_id_t         owner;
	cap_id_t             activity;
	struct redirect_slot input;
	struct redirect_slot output;
	bool                 running;
};

static struct redirect_server server = {
	.endpoint = CHANNEL_ID_INVALID,
	.activity = CAP_ID_INVALID,
	.input    = {.file = CAP_ID_INVALID, .stream = CAP_ID_INVALID},
	.output   = {.file = CAP_ID_INVALID, .stream = CAP_ID_INVALID},
};

static bool succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static const char* filesystem_error(struct filesystem_call_result result) {
	if (result.transport_status == SYSCALL_STATUS_DENIED) return "permission denied";
	if (result.transport_status != SYSCALL_STATUS_OK) return "filesystem transport error";
	switch (result.status) {
	case FILESYSTEM_STATUS_NOT_FOUND:
		return "no such file or directory";
	case FILESYSTEM_STATUS_ALREADY_EXISTS:
		return "file already exists";
	case FILESYSTEM_STATUS_NOT_DIRECTORY:
		return "not a directory";
	case FILESYSTEM_STATUS_IS_DIRECTORY:
		return "is a directory";
	case FILESYSTEM_STATUS_READ_ONLY:
		return "read-only filesystem";
	case FILESYSTEM_STATUS_ACCESS_DENIED:
		return "permission denied";
	case FILESYSTEM_STATUS_NO_SPACE:
		return "no space left on device";
	case FILESYSTEM_STATUS_INVALID_ARGUMENT:
		return "invalid path";
	case FILESYSTEM_STATUS_IO_ERROR:
		return "I/O error";
	default:
		return "filesystem operation failed";
	}
}

static bool reply(const struct cap_request* call, const void* bytes, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, bytes, size, status) == SYSCALL_STATUS_OK;
}

static void handle_request(const struct cap_request* call, const uint8_t* request) {
	struct redirect_slot*        slot = call->object_id == REDIRECT_INPUT_ID    ? &server.input
	                                    : call->object_id == REDIRECT_OUTPUT_ID ? &server.output
	                                                                            : NULL;
	struct stream_request_header header;
	if (slot == NULL || !slot->active || slot->file == CAP_ID_INVALID || call->request_size < sizeof(header)) {
		(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return;
	}
	memcpy(&header, request, sizeof(header));
	if (header.op == STREAM_OP_READ && slot == &server.input) {
		struct stream_read_request    read_request;
		uint8_t                       data[CAP_MAX_RESPONSE_SIZE];
		size_t                        count = 0u;
		struct filesystem_call_result result;
		if ((call->rights & (CAP_CALL | CAP_READ)) != (CAP_CALL | CAP_READ) ||
		    call->request_size != sizeof(read_request)) {
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
			return;
		}
		memcpy(&read_request, request, sizeof(read_request));
		if (read_request.reserved != 0u || read_request.size > call->response_capacity ||
		    read_request.size > sizeof(data) || read_request.size > UINT64_MAX - slot->offset) {
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
			return;
		}
		if (read_request.size == 0u) {
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_OK);
			return;
		}
		result = filesystem_file_read(slot->file, slot->offset, data, (size_t)read_request.size, &count);
		if (!succeeded(result) || count > read_request.size) {
			__atomic_store_n(&slot->failed, true, __ATOMIC_RELEASE);
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
			return;
		}
		slot->offset += count;
		(void)reply(call, data, count, SYSCALL_STATUS_OK);
		return;
	}
	if (header.op == STREAM_OP_WRITE && slot == &server.output) {
		struct stream_write_request   write_request;
		struct stream_write_response  response;
		struct filesystem_call_result result;
		size_t                        written = 0u;
		if ((call->rights & (CAP_CALL | CAP_WRITE)) != (CAP_CALL | CAP_WRITE) ||
		    call->request_size < sizeof(write_request)) {
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
			return;
		}
		memcpy(&write_request, request, sizeof(write_request));
		if (write_request.reserved != 0u || write_request.size > call->request_size - sizeof(write_request) ||
		    write_request.size != call->request_size - sizeof(write_request) ||
		    write_request.size > UINT64_MAX - slot->offset || call->response_capacity < sizeof(response)) {
			(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
			return;
		}
		if (write_request.size != 0u) {
			result = filesystem_file_write(
				slot->file, slot->offset, request + sizeof(write_request), (size_t)write_request.size, &written);
			if (!succeeded(result) || written == 0u || written > write_request.size) {
				__atomic_store_n(&slot->failed, true, __ATOMIC_RELEASE);
				(void)reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
				return;
			}
			slot->offset += written;
		}
		response.size = written;
		(void)reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
		return;
	}
	(void)reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
}

static void redirect_worker(void* unused) {
	static uint8_t buffer[CAP_MAX_REQUEST_SIZE];
	(void)unused;
	for (;;) {
		bool received;
		do {
			struct cap_request request;
			received = false;
			if (channel_recv(server.endpoint, &request, buffer, sizeof(buffer), &received) != SYSCALL_STATUS_OK) return;
			if (received) handle_request(&request, buffer);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			if (channel_event_recv(server.endpoint, &event, &received) != SYSCALL_STATUS_OK) return;
		} while (received);
		struct signal_message signal;
		if (signal_wait(server.activity, &signal) != SYSCALL_STATUS_OK) return;
	}
}

static bool start_server(void) {
	struct self_info self = {
		.self_cap          = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.main_thread_cap   = CAP_ID_INVALID,
	};
	if (server.running) return true;
	if (process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	if (self.self_cap == CAP_ID_INVALID || self.pid == PROCESS_PID_INVALID) {
		if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
		if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
		if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
		return false;
	}
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&server.endpoint, &server.activity) != SYSCALL_STATUS_OK) {
		(void)cap_drop(self.self_cap);
		return false;
	}
	bool spawned = process_spawn_detached_thread(self.self_cap, (uintptr_t)redirect_worker, NULL, 0u, NULL, 0u) ==
	               SYSCALL_STATUS_OK;
	(void)cap_drop(self.self_cap);
	if (!spawned) {
		(void)channel_destroy(server.endpoint);
		(void)cap_drop(server.activity);
		server.endpoint = CHANNEL_ID_INVALID;
		server.activity = CAP_ID_INVALID;
		return false;
	}
	server.owner   = self.pid;
	server.running = true;
	return true;
}

static bool discard_slot(struct redirect_slot* slot, uint64_t object_id) {
	bool ok = true;
	if (slot->stream != CAP_ID_INVALID) {
		if (cap_drop(slot->stream) != SYSCALL_STATUS_OK) ok = false;
		slot->stream = CAP_ID_INVALID;
		if (cap_unpublish(server.endpoint, object_id) != SYSCALL_STATUS_OK) ok = false;
	}
	if (slot->file != CAP_ID_INVALID) {
		if (cap_drop(slot->file) != SYSCALL_STATUS_OK) ok = false;
		slot->file = CAP_ID_INVALID;
	}
	slot->offset = 0u;
	slot->active = false;
	return ok;
}

bool shell_redirect_finish(void) {
	bool failed = __atomic_load_n(&server.input.failed, __ATOMIC_ACQUIRE) ||
	              __atomic_load_n(&server.output.failed, __ATOMIC_ACQUIRE);
	bool ok     = discard_slot(&server.input, REDIRECT_INPUT_ID);
	if (!discard_slot(&server.output, REDIRECT_OUTPUT_ID)) ok = false;
	__atomic_store_n(&server.input.failed, false, __ATOMIC_RELEASE);
	__atomic_store_n(&server.output.failed, false, __ATOMIC_RELEASE);
	return ok && !failed;
}

static bool open_redirect(const struct shell* shell, const struct shell_redirection* redirect,
                          struct redirect_slot* slot, const char** error_message) {
	char                          normalized[SHELL_PATH_CAPACITY];
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	size_t                        path_size = 0u;
	bool                          writing   = redirect->kind != SHELL_REDIRECT_INPUT;
	cap_rights_t                  rights    = writing ? CAP_WRITE : CAP_READ;
	if (!shell_resolve_path(shell, redirect->path, strlen(redirect->path), normalized, &path_size)) {
		*error_message = "invalid path";
		return false;
	}
	result = vfs_open(shell->vfs_cap, normalized, path_size, rights, &node);
	if (writing && result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_NOT_FOUND) {
		result = vfs_create_file(shell->vfs_cap, normalized, path_size, rights, &node);
	}
	if (!succeeded(result)) {
		*error_message = filesystem_error(result);
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		return false;
	}
	if (node.info.type != FILESYSTEM_NODE_FILE || node.capability == CAP_ID_INVALID) {
		*error_message = "is a directory";
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		return false;
	}
	if (redirect->kind == SHELL_REDIRECT_OUTPUT) {
		result = filesystem_file_resize(node.capability, 0u);
		if (!succeeded(result)) {
			*error_message = filesystem_error(result);
			(void)cap_drop(node.capability);
			return false;
		}
	}
	slot->file   = node.capability;
	slot->offset = redirect->kind == SHELL_REDIRECT_APPEND ? node.info.size : 0u;
	slot->active = true;
	return true;
}

static bool publish_slot(struct redirect_slot* slot, uint64_t id, cap_rights_t rights) {
	return cap_publish(server.endpoint, id, server.owner, rights | CAP_CALL | CAP_DELEGATE, &slot->stream) ==
	       SYSCALL_STATUS_OK;
}

bool shell_redirect_prepare(const struct shell* shell, const struct shell_command* command,
                            struct shell_redirect_streams* out_streams, const char** error_path,
                            const char** error_message) {
	if (out_streams == NULL || error_path == NULL || error_message == NULL || shell == NULL || command == NULL)
		return false;
	*out_streams   = (struct shell_redirect_streams){0};
	*error_path    = NULL;
	*error_message = "failed to initialize stream redirection";
	if (!start_server()) return false;
	/* The previous command is completely finished before entering this function. */
	(void)shell_redirect_finish();
	for (size_t i = 0u; i < command->redirection_count; i++) {
		const struct shell_redirection* redir = &command->redirections[i];
		struct redirect_slot*           slot  = redir->kind == SHELL_REDIRECT_INPUT ? &server.input : &server.output;
		*error_path                           = redir->path;
		if (!discard_slot(slot, slot == &server.input ? REDIRECT_INPUT_ID : REDIRECT_OUTPUT_ID) ||
		    !open_redirect(shell, redir, slot, error_message)) {
			(void)shell_redirect_finish();
			return false;
		}
	}
	if (server.input.active) {
		if (!publish_slot(&server.input, REDIRECT_INPUT_ID, CAP_READ)) goto fail;
		out_streams->input         = server.input.stream;
		out_streams->input_changed = true;
	}
	if (server.output.active) {
		if (!publish_slot(&server.output, REDIRECT_OUTPUT_ID, CAP_WRITE)) goto fail;
		out_streams->output         = server.output.stream;
		out_streams->output_changed = true;
	}
	return true;
fail:
	*error_path    = NULL;
	*error_message = "failed to publish redirected stream";
	(void)shell_redirect_finish();
	return false;
}
