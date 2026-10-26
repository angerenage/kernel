#include <base/channel.h>
#include <base/process.h>
#include <runtime/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#include "init.h"

#define SERIAL_STREAM_OBJECT_ID 1u

static channel_id_t serial_stream_endpoint = CHANNEL_ID_INVALID;
static cap_id_t     serial_stream_activity = CAP_ID_INVALID;
static cap_id_t     serial_stream_serial   = CAP_ID_INVALID;
static uint8_t      serial_stream_request[CAP_MAX_REQUEST_SIZE];

static bool serial_stream_reply(const struct cap_request* call, const void* response, size_t response_size,
                                syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool serial_stream_dispatch(const struct cap_request* call, const void* data) {
	struct stream_request_header header;
	struct stream_write_request  request;
	struct stream_write_response response;
	syscall_status_t             status;

	if (call->object_id != SERIAL_STREAM_OBJECT_ID || data == NULL || call->request_size < sizeof(header)) {
		return serial_stream_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	memcpy(&header, data, sizeof(header));
	if (header.op == STREAM_OP_READ) {
		status = (call->rights & CAP_READ) == 0u ? SYSCALL_STATUS_DENIED : SYSCALL_STATUS_UNAVAILABLE;
		return serial_stream_reply(call, NULL, 0u, status);
	}
	if (header.op != STREAM_OP_WRITE) return serial_stream_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((call->rights & CAP_WRITE) == 0u) return serial_stream_reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(request) || call->response_capacity < sizeof(response))
		return serial_stream_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    call->request_size != sizeof(request) + (size_t)request.size)
		return serial_stream_reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.size != 0u) {
		syscall_result_t result = cap_call_syscall(
			serial_stream_serial, (const uint8_t*)data + sizeof(request), (size_t)request.size, NULL, 0u);
		status = result.status;
		if (status != SYSCALL_STATUS_OK) return serial_stream_reply(call, NULL, 0u, status);
	}
	response.size = request.size;
	return serial_stream_reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static void serial_stream_server(void* unused) {
	(void)unused;
	for (;;) {
		struct cap_request call;
		bool               received;
		syscall_status_t   status;

		do {
			received = false;
			status   = channel_recv(
				serial_stream_endpoint, &call, serial_stream_request, sizeof(serial_stream_request), &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
			if (received && !serial_stream_dispatch(&call, serial_stream_request))
				process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(serial_stream_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		} while (received);
		struct signal_message activity;
		if (signal_wait(serial_stream_activity, &activity) != SYSCALL_STATUS_OK)
			process_exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
}

syscall_status_t serial_stream_create(cap_id_t serial_cap, cap_id_t* out_stream_cap) {
	struct self_info self;
	cap_id_t         stream_cap = CAP_ID_INVALID;
	cap_id_t         thread_cap = CAP_ID_INVALID;
	syscall_status_t status;

	if (serial_cap == CAP_ID_INVALID || out_stream_cap == NULL || serial_stream_endpoint != CHANNEL_ID_INVALID)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_stream_cap = CAP_ID_INVALID;
	status          = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) return status;
	status = channel_create(&serial_stream_endpoint, &serial_stream_activity);
	if (status != SYSCALL_STATUS_OK) return status;
	serial_stream_serial = serial_cap;
	status               = cap_publish(serial_stream_endpoint,
	                                   SERIAL_STREAM_OBJECT_ID,
	                                   self.pid,
	                                   CAP_CALL | CAP_WRITE | CAP_REVOKE | CAP_DELEGATE,
	                                   &stream_cap);
	if (status == SYSCALL_STATUS_OK) {
		status = process_spawn_thread(self.self_cap,
		                              (uintptr_t)serial_stream_server,
		                              NULL,
		                              0u,
		                              "init/serial-stream",
		                              sizeof("init/serial-stream"),
		                              &thread_cap);
	}
	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK && drop_status != SYSCALL_STATUS_OK) status = drop_status;
	}
	if (status == SYSCALL_STATUS_OK) {
		*out_stream_cap = stream_cap;
		return SYSCALL_STATUS_OK;
	}
	if (stream_cap != CAP_ID_INVALID) (void)cap_drop(stream_cap);
	(void)channel_destroy(serial_stream_endpoint);
	serial_stream_endpoint = CHANNEL_ID_INVALID;
	serial_stream_activity = CAP_ID_INVALID;
	serial_stream_serial   = CAP_ID_INVALID;
	return status;
}
