#include "server.h"

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <protocol/terminal.h>
#include <runtime/init.h>
#include <runtime/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "input.h"
#include "terminal.h"

#define TERMINAL_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_PROTOCOL_VERSION_MINOR 0u
#define TERMINAL_SERVICE_OBJECT_ID 1u

static bool reply_request(const struct cap_request* call, const void* response, size_t response_size,
                          syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool dispatch_request(struct terminal* terminal, struct terminal_input* input, const struct cap_request* call,
                             const uint8_t* data) {
	struct stream_request_header header;
	struct stream_write_request  request;
	struct stream_write_response response;
	size_t                       written;
	bool                         complete;

	if (terminal == NULL || call->object_id != TERMINAL_SERVICE_OBJECT_ID || data == NULL ||
	    call->request_size < sizeof(header))
		return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	if (header.op == TERMINAL_OP_GET_INFO) {
		struct terminal_get_info_response info;
		size_t                            columns;
		size_t                            rows;
		if ((call->rights & CAP_READ) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_get_info_request) || call->response_capacity < sizeof(info) ||
		    !terminal_get_dimensions(terminal, &columns, &rows))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		info = (struct terminal_get_info_response){.columns = columns, .rows = rows};
		return reply_request(call, &info, sizeof(info), SYSCALL_STATUS_OK);
	}
	if (header.op == TERMINAL_OP_GET_CURSOR) {
		struct terminal_get_cursor_response cursor;
		size_t                              column;
		size_t                              row;
		uint64_t                            line;
		if ((call->rights & CAP_READ) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_get_cursor_request) ||
		    call->response_capacity < sizeof(cursor) || !terminal_get_cursor_position(terminal, &column, &row, &line))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		cursor = (struct terminal_get_cursor_response){.column = column, .row = row, .line = line};
		return reply_request(call, &cursor, sizeof(cursor), SYSCALL_STATUS_OK);
	}
	if (header.op == TERMINAL_OP_GET_SELECTION) {
		struct terminal_get_selection_response selection;
		if ((call->rights & CAP_READ) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_get_selection_request) ||
		    call->response_capacity < sizeof(selection))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		if (!terminal_selection_get(terminal, &selection))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
		return reply_request(call, &selection, sizeof(selection), SYSCALL_STATUS_OK);
	}
	if (header.op == TERMINAL_OP_SET_SELECTION) {
		struct terminal_set_selection_request selection;
		if ((call->rights & CAP_WRITE) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(selection)) return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		memcpy(&selection, data, sizeof(selection));
		if (selection.anchor.line == 0u || selection.focus.line == 0u ||
		    (selection.anchor.line == selection.focus.line && selection.anchor.column == selection.focus.column) ||
		    !terminal_selection_set(terminal, selection.anchor, selection.focus))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_request(call, NULL, 0u, SYSCALL_STATUS_OK);
	}
	if (header.op == TERMINAL_OP_CLEAR_SELECTION) {
		if ((call->rights & CAP_WRITE) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_clear_selection_request))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_request(
			call, NULL, 0u, terminal_selection_clear(terminal) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED);
	}
	if (header.op == TERMINAL_OP_ACQUIRE_INTERRUPT_SIGNAL) {
		struct terminal_acquire_interrupt_signal_response response = {.capability = CAP_ID_INVALID};
		if ((call->rights & CAP_MANAGE) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_acquire_interrupt_signal_request) ||
		    call->response_capacity < sizeof(response))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		if (!terminal_input_acquire_interrupt_signal(input, call->caller, &response.capability))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_FAILED);
		bool success = reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
		if (!success) (void)cap_revoke(response.capability, 0u);
		return success;
	}
	if (header.op == TERMINAL_OP_DISCARD_INPUT) {
		struct terminal_discard_input_response response;
		if ((call->rights & CAP_MANAGE) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		if (call->request_size != sizeof(struct terminal_discard_input_request) ||
		    call->response_capacity < sizeof(response))
			return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.interrupt_generation = terminal_input_discard(input);
		return reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	if (header.op == STREAM_OP_READ) {
		if ((call->rights & CAP_READ) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		return terminal_input_read(input, call, data);
	}
	if (header.op != STREAM_OP_WRITE) return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((call->rights & CAP_WRITE) == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(request) || call->response_capacity < sizeof(response))
		return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    call->request_size != sizeof(request) + (size_t)request.size)
		return reply_request(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	complete = terminal_write(terminal, data + sizeof(request), (size_t)request.size, &written);
	if (!complete && written == 0u) return reply_request(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	response.size = written;
	return reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool advertise_terminal(channel_id_t* out_endpoint, cap_id_t* out_activity) {
	const struct init_service_selector selector = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.service        = TERMINAL_SERVICE_NAME,
	};
	struct self_info        self;
	struct init_call_result result;
	cap_id_t                service_cap = CAP_ID_INVALID;
	syscall_status_t        status;

	if (out_endpoint == NULL || out_activity == NULL) return false;
	*out_endpoint = CHANNEL_ID_INVALID;
	*out_activity = CAP_ID_INVALID;
	status        = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	status = channel_create(out_endpoint, out_activity);
	if (status != SYSCALL_STATUS_OK) return false;
	status = cap_publish(*out_endpoint,
	                     TERMINAL_SERVICE_OBJECT_ID,
	                     self.pid,
	                     TERMINAL_CLIENT_RIGHTS | CAP_MANAGE | CAP_DELEGATE_PEER,
	                     &service_cap);
	if (status != SYSCALL_STATUS_OK) goto fail;
	result = init_advertise_with_rights(&selector,
	                                    TERMINAL_PROTOCOL_VERSION_MINOR,
	                                    service_cap,
	                                    TERMINAL_CLIENT_RIGHTS | CAP_MANAGE,
	                                    TERMINAL_CLIENT_RIGHTS);
	if (result.status != INIT_REGISTRY_OK || result.transport_status != SYSCALL_STATUS_OK) goto fail;
	return true;

fail:
	if (service_cap != CAP_ID_INVALID) (void)cap_drop(service_cap);
	(void)channel_destroy(*out_endpoint);
	*out_endpoint = CHANNEL_ID_INVALID;
	*out_activity = CAP_ID_INVALID;
	return false;
}

int terminal_server_run(struct terminal* terminal) {
	static uint8_t         request_data[CAP_MAX_REQUEST_SIZE];
	channel_id_t           endpoint;
	cap_id_t               activity_cap;
	struct terminal_input* input  = NULL;
	int                    result = 1;

	if (terminal == NULL || (input = terminal_input_create(terminal)) == NULL) {
		printf("terminal: input allocation failed\n");
		goto cleanup;
	}
	if (!terminal_input_start(input)) {
		printf("terminal: input startup failed\n");
		goto cleanup;
	}
	if (!advertise_terminal(&endpoint, &activity_cap)) {
		printf("terminal: advertisement failed\n");
		goto cleanup;
	}
	for (;;) {
		bool             received;
		syscall_status_t status;

		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(endpoint, &call, request_data, sizeof(request_data), &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch_request(terminal, input, &call, request_data)))
				goto cleanup;
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) goto cleanup;
		} while (received);
		struct signal_message activity;
		if (signal_wait(activity_cap, &activity) != SYSCALL_STATUS_OK) goto cleanup;
	}

cleanup:
	terminal_input_destroy(input);
	return result;
}
