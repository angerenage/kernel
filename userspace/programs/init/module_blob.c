#include "module_blob.h"

#include <base/channel.h>
#include <base/process.h>
#include <runtime/blob.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/module.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#define MODULE_BLOB_OBJECT_ID 1u

static channel_id_t module_blob_endpoint = CHANNEL_ID_INVALID;
static cap_id_t     module_blob_activity = CAP_ID_INVALID;
static cap_id_t     module_blob_module   = CAP_ID_INVALID;
static uint64_t     module_blob_size;
static uint8_t      module_blob_response[CAP_MAX_RESPONSE_SIZE];

static bool module_blob_reply_info(const struct cap_request* call) {
	const struct blob_info_response response = {.size = module_blob_size};

	if (call->request_size != sizeof(struct blob_info_request) || call->response_capacity < sizeof(response)) {
		return channel_reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) == SYSCALL_STATUS_OK;
	}
	return channel_reply(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK) == SYSCALL_STATUS_OK;
}

static bool module_blob_reply_read(const struct cap_request* call, const void* data) {
	struct blob_read_request request;
	syscall_status_t         status;

	if (call->request_size != sizeof(request)) {
		return channel_reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) == SYSCALL_STATUS_OK;
	}
	memcpy(&request, data, sizeof(request));
	if (request.size > CAP_MAX_RESPONSE_SIZE || request.size > call->response_capacity ||
	    request.offset > module_blob_size || request.size > module_blob_size - request.offset) {
		return channel_reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) == SYSCALL_STATUS_OK;
	}
	status = module_read(module_blob_module, request.offset, module_blob_response, (size_t)request.size);
	return channel_reply(call->call_id,
	                     status == SYSCALL_STATUS_OK ? module_blob_response : NULL,
	                     status == SYSCALL_STATUS_OK ? (size_t)request.size : 0u,
	                     status) == SYSCALL_STATUS_OK;
}

static bool module_blob_dispatch(const struct cap_request* call, const void* data) {
	struct blob_request_header header;

	if (call->object_id != MODULE_BLOB_OBJECT_ID || (call->rights & CAP_READ) == 0u || data == NULL ||
	    call->request_size < sizeof(header)) {
		return channel_reply(call->call_id,
		                     NULL,
		                     0u,
		                     (call->rights & CAP_READ) == 0u ? SYSCALL_STATUS_DENIED : SYSCALL_STATUS_BAD_ARGUMENT) ==
		       SYSCALL_STATUS_OK;
	}
	memcpy(&header, data, sizeof(header));
	if (header.op == BLOB_OP_INFO) return module_blob_reply_info(call);
	if (header.op == BLOB_OP_READ) return module_blob_reply_read(call, data);
	return channel_reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) == SYSCALL_STATUS_OK;
}

static void module_blob_server(void* unused) {
	struct blob_read_request buffer;

	(void)unused;
	for (;;) {
		struct cap_request call;
		bool               received;
		syscall_status_t   status;

		do {
			received = false;
			status   = channel_recv(module_blob_endpoint, &call, &buffer, sizeof(buffer), &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
			if (received && !module_blob_dispatch(&call, &buffer))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(module_blob_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		if (signal_wait(module_blob_activity, &activity) != SYSCALL_STATUS_OK)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

syscall_status_t module_blob_create(cap_id_t module_cap, size_t module_size, cap_id_t* out_blob_cap) {
	struct self_info self;
	cap_id_t         blob_cap   = CAP_ID_INVALID;
	cap_id_t         thread_cap = CAP_ID_INVALID;
	syscall_status_t status;

	if (module_cap == CAP_ID_INVALID || out_blob_cap == NULL || module_blob_endpoint != CHANNEL_ID_INVALID)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_blob_cap = CAP_ID_INVALID;
	status        = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) return status;
	status = channel_create(&module_blob_endpoint, &module_blob_activity);
	if (status != SYSCALL_STATUS_OK) return status;
	module_blob_module = module_cap;
	module_blob_size   = module_size;
	status             = cap_publish(module_blob_endpoint,
	                                 MODULE_BLOB_OBJECT_ID,
	                                 self.pid,
	                                 CAP_CALL | CAP_READ | CAP_REVOKE | CAP_DELEGATE,
	                                 &blob_cap);
	if (status == SYSCALL_STATUS_OK) {
		status = process_spawn_thread(self.self_cap,
		                              (uintptr_t)module_blob_server,
		                              NULL,
		                              0u,
		                              "init/module-blob",
		                              sizeof("init/module-blob"),
		                              &thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK && drop_status != SYSCALL_STATUS_OK) status = drop_status;
	}
	if (status == SYSCALL_STATUS_OK) {
		*out_blob_cap = blob_cap;
		return SYSCALL_STATUS_OK;
	}
	if (blob_cap != CAP_ID_INVALID) (void)cap_drop(blob_cap);
	(void)channel_destroy(module_blob_endpoint);
	module_blob_endpoint = CHANNEL_ID_INVALID;
	module_blob_activity = CAP_ID_INVALID;
	module_blob_module   = CAP_ID_INVALID;
	module_blob_size     = 0u;
	return status;
}
