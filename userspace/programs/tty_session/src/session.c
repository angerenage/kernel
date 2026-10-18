#include "session.h"

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <protocol/terminal.h>
#include <protocol/tty_session.h>
#include <runtime/init.h>
#include <runtime/stream.h>
#include <runtime/terminal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/display.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#define TTY_SESSION_PROTOCOL_VERSION_MAJOR 1u
#define TTY_SESSION_PROTOCOL_VERSION_MINOR 0u
#define TTY_SESSION_SERVICE_OBJECT_ID 1u
#define TTY_SESSION_INPUT_CAPACITY 4096u
#define TTY_SESSION_PENDING_CAPACITY 64u
#define TTY_SESSION_INTERRUPT_BYTE 0x03u

struct pending_read {
	cap_call_id_t call_id;
	process_id_t  caller;
	size_t        size;
};

struct tty_session {
	cap_id_t            self_cap;
	cap_id_t            terminal_cap;
	channel_id_t        endpoint;
	cap_id_t            activity_cap;
	cap_id_t            input_space_cap;
	cap_id_t            service_cap;
	process_id_t        session_pid;
	process_id_t        foreground_pid;
	cap_id_t            foreground_process_cap;
	bool                foreground_interrupt_pending;
	uint8_t             input[TTY_SESSION_INPUT_CAPACITY];
	size_t              input_head;
	size_t              input_count;
	struct pending_read pending[TTY_SESSION_PENDING_CAPACITY];
	size_t              pending_head;
	size_t              pending_count;
	bool                locked;
};

static void fail_session(struct tty_session* session, const char* message);

/* Serialize input, pending reads, and foreground ownership. */
static void session_lock(struct tty_session* session) {
	while (__atomic_test_and_set(&session->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

/* Release the session state lock. */
static void session_unlock(struct tty_session* session) {
	__atomic_clear(&session->locked, __ATOMIC_RELEASE);
}

/* Complete one request through the session endpoint. */
static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

/* Copy queued input without consuming it. The caller holds the state lock. */
static size_t copy_input(const struct tty_session* session, uint8_t* output, size_t capacity) {
	size_t count = session->input_count < capacity ? session->input_count : capacity;
	for (size_t i = 0u; i < count; i++)
		output[i] = session->input[(session->input_head + i) % TTY_SESSION_INPUT_CAPACITY];
	return count;
}

/* Consume bytes and report whether a full queue became writable. */
static bool consume_input(struct tty_session* session, size_t count) {
	bool was_full       = session->input_count == TTY_SESSION_INPUT_CAPACITY;
	session->input_head = (session->input_head + count) % TTY_SESSION_INPUT_CAPACITY;
	session->input_count -= count;
	return was_full && count != 0u;
}

/* Wake the terminal-input thread after a reader releases queue capacity. */
static bool notify_input_space(struct tty_session* session) {
	struct signal_send_response response;

	return signal_send(session->input_space_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	       SYSCALL_STATUS_OK;
}

/* Drop a process capability unless reaping the process already destroyed it. */
static bool drop_process_capability(cap_id_t capability) {
	bool valid;

	if (capability == CAP_ID_INVALID) return true;
	if (cap_valid(capability, &valid) != SYSCALL_STATUS_OK) return false;
	return !valid || cap_drop(capability) == SYSCALL_STATUS_OK;
}

/* Complete queued reads belonging to the current foreground process. */
static void service_pending_reads(struct tty_session* session) {
	uint8_t response[CAP_MAX_RESPONSE_SIZE];
	bool    space_available = false;

	session_lock(session);
	while (session->pending_count != 0u && session->input_count != 0u) {
		struct pending_read* pending = &session->pending[session->pending_head];
		if (session->foreground_pid != PROCESS_PID_INVALID && pending->caller != session->foreground_pid) break;
		size_t count = copy_input(session, response, pending->size);
		if (channel_reply(pending->call_id, response, count, SYSCALL_STATUS_OK) == SYSCALL_STATUS_OK &&
		    consume_input(session, count))
			space_available = true;
		session->pending_head = (session->pending_head + 1u) % TTY_SESSION_PENDING_CAPACITY;
		session->pending_count--;
	}
	session_unlock(session);
	if (space_available && !notify_input_space(session))
		fail_session(session, "input availability notification failed");
}

/* Reject reads left behind by the previous foreground process. */
static void reject_pending_reads(struct tty_session* session) {
	while (session->pending_count != 0u) {
		struct pending_read* pending = &session->pending[session->pending_head];
		(void)channel_reply(pending->call_id, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
		session->pending_head = (session->pending_head + 1u) % TTY_SESSION_PENDING_CAPACITY;
		session->pending_count--;
	}
}

/* Terminate the session after an unrecoverable terminal-input failure. */
static void fail_session(struct tty_session* session, const char* message) {
	printf("tty session: %s\n", message);
	(void)process_kill(session->self_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	thread_exit(1u);
}

/* Acquire the state lock only after the input queue has writable capacity. */
static void lock_input_space(struct tty_session* session) {
	for (;;) {
		struct signal_message message;

		session_lock(session);
		if (session->input_count != TTY_SESSION_INPUT_CAPACITY) return;
		session_unlock(session);
		if (signal_wait(session->input_space_cap, &message) != SYSCALL_STATUS_OK)
			fail_session(session, "input availability wait failed");
	}
}

/* Consume terminal input continuously and route interrupts at the session boundary. */
static void input_thread(void* opaque) {
	struct tty_session* session = *(struct tty_session**)opaque;

	for (;;) {
		uint8_t byte;
		size_t  size = 0u;
		lock_input_space(session);
		session_unlock(session);
		if (display_read(&byte, 1u, &size) != SYSCALL_STATUS_OK || size != 1u)
			fail_session(session, "terminal input failed");

		session_lock(session);
		if (byte == TTY_SESSION_INTERRUPT_BYTE && session->foreground_pid != PROCESS_PID_INVALID) {
			if (session->foreground_process_cap == CAP_ID_INVALID) {
				session->foreground_interrupt_pending = true;
				session_unlock(session);
				continue;
			}
			syscall_status_t status = process_kill(session->foreground_process_cap, PROCESS_EXIT_SYSTEM_INTERRUPTED);
			session_unlock(session);
			if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE)
				fail_session(session, "foreground interrupt failed");
			continue;
		}
		size_t tail          = (session->input_head + session->input_count) % TTY_SESSION_INPUT_CAPACITY;
		session->input[tail] = byte;
		session->input_count++;
		session_unlock(session);
		service_pending_reads(session);
	}
}

/* Handle one generic Stream read from the foreground process. */
static bool dispatch_read(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct stream_read_request request;
	uint8_t                    response[CAP_MAX_RESPONSE_SIZE];

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.header.op != STREAM_OP_READ || request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE ||
	    request.size > call->response_capacity)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.size == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_OK);

	session_lock(session);
	if (session->foreground_pid == PROCESS_PID_INVALID) {
		if ((call->rights & CAP_MANAGE) == 0u) {
			session_unlock(session);
			return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
		}
	}
	else if (call->caller != session->foreground_pid) {
		session_unlock(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	if (session->input_count != 0u) {
		size_t count           = copy_input(session, response, (size_t)request.size);
		bool   success         = reply(call, response, count, SYSCALL_STATUS_OK);
		bool   space_available = success && consume_input(session, count);
		session_unlock(session);
		return success && (!space_available || notify_input_space(session));
	}
	if (session->pending_count == TTY_SESSION_PENDING_CAPACITY) {
		session_unlock(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	size_t tail            = (session->pending_head + session->pending_count) % TTY_SESSION_PENDING_CAPACITY;
	session->pending[tail] = (struct pending_read){
		.call_id = call->call_id,
		.caller  = call->caller,
		.size    = (size_t)request.size,
	};
	session->pending_count++;
	session_unlock(session);
	return true;
}

/* Forward a terminal-dimensions query through the session. */
static bool dispatch_get_info(struct tty_session* session, const struct cap_request* call) {
	struct terminal_get_info_response    terminal_info;
	struct tty_session_get_info_response response;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_get_info_request) || call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status = terminal_get_info(session->terminal_cap, &terminal_info);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response = (struct tty_session_get_info_response){
		.columns     = terminal_info.columns,
		.rows        = terminal_info.rows,
		.session_pid = session->session_pid,
	};
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

/* Forward a terminal-cursor query through the session. */
static bool dispatch_get_cursor(struct tty_session* session, const struct cap_request* call) {
	struct terminal_get_cursor_response    terminal_cursor;
	struct tty_session_get_cursor_response response;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_get_cursor_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status = terminal_get_cursor(session->terminal_cap, &terminal_cursor);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response = (struct tty_session_get_cursor_response){
		.column = terminal_cursor.column,
		.row    = terminal_cursor.row,
	};
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

/* Select a prepared child before it starts running. */
static bool dispatch_begin_foreground(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct tty_session_begin_foreground_request request;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.process_id == PROCESS_PID_INVALID) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	session_lock(session);
	if (session->foreground_pid != PROCESS_PID_INVALID || request.process_id == call->caller) {
		session_unlock(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	bool success = reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (success) {
		reject_pending_reads(session);
		session->foreground_pid               = request.process_id;
		session->foreground_interrupt_pending = false;
	}
	session_unlock(session);
	return success;
}

/* Install the process-control capability after the selected child starts. */
static bool dispatch_attach_foreground(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct tty_session_attach_foreground_request request;
	struct process_info_response                 info;
	bool                                         interrupt_pending = false;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.process_cap == CAP_ID_INVALID || request.process_id == PROCESS_PID_INVALID ||
	    process_get_info(request.process_cap, &info) != SYSCALL_STATUS_OK || info.pid != request.process_id)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	session_lock(session);
	if (session->foreground_process_cap != CAP_ID_INVALID || request.process_id != session->foreground_pid ||
	    request.process_id == call->caller) {
		session_unlock(session);
		return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	}
	bool success = reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (success) {
		session->foreground_process_cap       = request.process_cap;
		interrupt_pending                     = session->foreground_interrupt_pending;
		session->foreground_interrupt_pending = false;
	}
	session_unlock(session);
	if (success && interrupt_pending) (void)process_kill(request.process_cap, PROCESS_EXIT_SYSTEM_INTERRUPTED);
	return success;
}

/* Return foreground ownership to the controlling shell. */
static bool dispatch_restore_foreground(struct tty_session* session, const struct cap_request* call) {
	cap_id_t process_cap = CAP_ID_INVALID;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_restore_foreground_request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	session_lock(session);
	bool success = reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (success) {
		reject_pending_reads(session);
		process_cap                           = session->foreground_process_cap;
		session->foreground_process_cap       = CAP_ID_INVALID;
		session->foreground_pid               = PROCESS_PID_INVALID;
		session->foreground_interrupt_pending = false;
	}
	session_unlock(session);
	if (success && !drop_process_capability(process_cap))
		printf("tty session: foreground process capability drop failed\n");
	return success;
}

/* Decode and dispatch one request to the session object. */
static bool dispatch_request(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct tty_session_request_header header;

	if (session == NULL || call->object_id != TTY_SESSION_SERVICE_OBJECT_ID || data == NULL ||
	    call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	switch ((uint32_t)header.op) {
	case STREAM_OP_READ:
		return dispatch_read(session, call, data);
	case STREAM_OP_WRITE:
		return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	case TTY_SESSION_OP_GET_INFO:
		return dispatch_get_info(session, call);
	case TTY_SESSION_OP_GET_CURSOR:
		return dispatch_get_cursor(session, call);
	case TTY_SESSION_OP_BEGIN_FOREGROUND:
		return dispatch_begin_foreground(session, call, data);
	case TTY_SESSION_OP_ATTACH_FOREGROUND:
		return dispatch_attach_foreground(session, call, data);
	case TTY_SESSION_OP_RESTORE_FOREGROUND:
		return dispatch_restore_foreground(session, call);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

/* Create and advertise the public session capability. */
static bool advertise_session(struct tty_session* session, process_id_t pid) {
	const struct init_service_selector selector = {
		.namespace_path = TTY_SESSION_NAMESPACE,
		.protocol       = TTY_SESSION_PROTOCOL_NAME,
		.major          = TTY_SESSION_PROTOCOL_VERSION_MAJOR,
		.service        = TTY_SESSION_SERVICE_NAME,
	};
	struct init_call_result result;

	if (channel_create(&session->endpoint, &session->activity_cap) != SYSCALL_STATUS_OK) return false;
	if (cap_publish(
			session->endpoint, TTY_SESSION_SERVICE_OBJECT_ID, pid, TTY_SESSION_CONTROL_RIGHTS, &session->service_cap) !=
	    SYSCALL_STATUS_OK)
		return false;
	result = init_advertise_with_rights(&selector,
	                                    TTY_SESSION_PROTOCOL_VERSION_MINOR,
	                                    session->service_cap,
	                                    TTY_SESSION_CONTROL_RIGHTS,
	                                    TTY_SESSION_CLIENT_RIGHTS);
	return result.transport_status == SYSCALL_STATUS_OK && result.status == INIT_REGISTRY_OK;
}

int tty_session_run(void) {
	static uint8_t     request_data[CAP_MAX_REQUEST_SIZE];
	struct tty_session session = {
		.self_cap               = CAP_ID_INVALID,
		.terminal_cap           = CAP_ID_INVALID,
		.endpoint               = CHANNEL_ID_INVALID,
		.activity_cap           = CAP_ID_INVALID,
		.input_space_cap        = CAP_ID_INVALID,
		.service_cap            = CAP_ID_INVALID,
		.session_pid            = PROCESS_PID_INVALID,
		.foreground_pid         = PROCESS_PID_INVALID,
		.foreground_process_cap = CAP_ID_INVALID,
	};
	struct self_info self;
	cap_id_t         input_thread_cap = CAP_ID_INVALID;

	display_get_standard_streams(&session.terminal_cap, NULL, NULL);
	if (session.terminal_cap == CAP_ID_INVALID || process_self_info(&self) != SYSCALL_STATUS_OK ||
	    self.pid == PROCESS_PID_INVALID || self.self_cap == CAP_ID_INVALID) {
		printf("tty session: initialization failed\n");
		return 1;
	}
	session.self_cap    = self.self_cap;
	session.session_pid = self.pid;
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (signal_create(&session.input_space_cap) != SYSCALL_STATUS_OK || !advertise_session(&session, self.pid)) {
		printf("tty session: advertisement failed\n");
		return 1;
	}
	struct tty_session* argument = &session;
	if (process_spawn_thread(
			session.self_cap, (uintptr_t)input_thread, &argument, sizeof(argument), NULL, 0u, &input_thread_cap) !=
	        SYSCALL_STATUS_OK ||
	    thread_detach(input_thread_cap) != SYSCALL_STATUS_OK) {
		printf("tty session: input thread startup failed\n");
		return 1;
	}

	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(session.endpoint, &call, request_data, sizeof(request_data), &received);
			if (status != SYSCALL_STATUS_OK || (received && !dispatch_request(&session, &call, request_data))) return 1;
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(session.endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
		} while (received);
		struct signal_message activity;
		if (signal_wait(session.activity_cap, &activity) != SYSCALL_STATUS_OK) return 1;
	}
}
