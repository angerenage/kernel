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
	cap_id_t            interrupt_signal_cap;
	cap_id_t            service_cap;
	process_id_t        session_pid;
	process_id_t        foreground_pid;
	cap_id_t            foreground_process_cap;
	cap_id_t            retired_process_cap;
	cap_id_t            busy_process_cap;
	bool                foreground_interrupt_pending;
	bool                input_paused;
	uint64_t            input_epoch;
	uint64_t            interrupt_generation;
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

/* Discard bytes owned by the previous foreground epoch. */
static bool discard_input(struct tty_session* session) {
	bool was_full        = session->input_count == TTY_SESSION_INPUT_CAPACITY;
	session->input_head  = 0u;
	session->input_count = 0u;
	return was_full;
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
		if (!session->input_paused && session->input_count != TTY_SESSION_INPUT_CAPACITY) return;
		session_unlock(session);
		if (signal_wait(session->input_space_cap, &message) != SYSCALL_STATUS_OK)
			fail_session(session, "input availability wait failed");
	}
}

/* Pause byte ingestion and flush the terminal's upstream queue for a foreground transition. */
static bool pause_and_discard_input(struct tty_session* session) {
	uint64_t interrupt_generation;

	session_lock(session);
	session->input_paused = true;
	session->input_epoch++;
	if (session->input_epoch == 0u) session->input_epoch = 1u;
	session_unlock(session);
	if (terminal_discard_input(session->terminal_cap, &interrupt_generation) == SYSCALL_STATUS_OK) {
		session_lock(session);
		session->interrupt_generation = interrupt_generation;
		session_unlock(session);
		return true;
	}
	session_lock(session);
	session->input_paused = false;
	session_unlock(session);
	(void)notify_input_space(session);
	return false;
}

/* Compare wrapping nonzero interrupt generations within their practical half-range. */
static bool interrupt_generation_after(uint64_t generation, uint64_t previous) {
	return generation != 0u && generation != previous && generation - previous < ((uint64_t)1u << 63u);
}

/* Attempt to terminate a process capability that may already have been reaped. */
static syscall_status_t interrupt_process(cap_id_t process_cap) {
	bool valid;

	if (process_cap == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
	if (cap_valid(process_cap, &valid) != SYSCALL_STATUS_OK) return SYSCALL_STATUS_FAILED;
	if (!valid) return SYSCALL_STATUS_OK;
	return process_kill(process_cap, PROCESS_EXIT_SYSTEM_INTERRUPTED);
}

/* Release a process capability retired while an interrupt was in progress. */
static void finish_process_interrupt(struct tty_session* session, cap_id_t process_cap) {
	for (;;) {
		cap_id_t retired = CAP_ID_INVALID;
		cap_id_t next    = CAP_ID_INVALID;

		session_lock(session);
		if (session->busy_process_cap == process_cap) session->busy_process_cap = CAP_ID_INVALID;
		if (session->retired_process_cap == process_cap) {
			retired                      = session->retired_process_cap;
			session->retired_process_cap = CAP_ID_INVALID;
		}
		if (session->foreground_interrupt_pending && session->foreground_process_cap != CAP_ID_INVALID &&
		    session->busy_process_cap == CAP_ID_INVALID) {
			next                                  = session->foreground_process_cap;
			session->busy_process_cap             = next;
			session->foreground_interrupt_pending = false;
		}
		session_unlock(session);
		if (!drop_process_capability(retired)) printf("tty session: retired process capability drop failed\n");
		if (next == CAP_ID_INVALID) return;
		syscall_status_t status = interrupt_process(next);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE &&
		    status != SYSCALL_STATUS_BAD_ARGUMENT)
			printf("tty session: foreground interrupt failed: %u\n", (unsigned)status);
		process_cap = next;
	}
}

/* Consume ordinary terminal input continuously. */
static void input_thread(void* opaque) {
	struct tty_session* session = *(struct tty_session**)opaque;

	for (;;) {
		uint8_t          byte;
		size_t           size = 0u;
		uint64_t         epoch;
		syscall_status_t status;
		lock_input_space(session);
		epoch = session->input_epoch;
		session_unlock(session);
		status = display_read(&byte, 1u, &size);
		if (status == SYSCALL_STATUS_UNAVAILABLE) continue;
		if (status != SYSCALL_STATUS_OK || size != 1u) fail_session(session, "terminal input failed");

		session_lock(session);
		if (session->input_paused || session->input_epoch != epoch) {
			session_unlock(session);
			continue;
		}
		size_t tail          = (session->input_head + session->input_count) % TTY_SESSION_INPUT_CAPACITY;
		session->input[tail] = byte;
		session->input_count++;
		session_unlock(session);
		service_pending_reads(session);
	}
}

/* Route interrupt characters independently of ordinary input backpressure. */
static void interrupt_thread(void* opaque) {
	struct tty_session* session = *(struct tty_session**)opaque;

	for (;;) {
		struct signal_message message;
		cap_id_t              process_cap = CAP_ID_INVALID;
		bool                  notify_space;

		if (signal_wait(session->interrupt_signal_cap, &message) != SYSCALL_STATUS_OK)
			fail_session(session, "terminal interrupt wait failed");
		session_lock(session);
		if (session->input_paused ||
		    !interrupt_generation_after(message.payload.args[0], session->interrupt_generation)) {
			session_unlock(session);
			continue;
		}
		session->interrupt_generation = message.payload.args[0];
		notify_space                  = discard_input(session);
		if (session->foreground_pid == PROCESS_PID_INVALID) {
			size_t tail          = (session->input_head + session->input_count) % TTY_SESSION_INPUT_CAPACITY;
			session->input[tail] = TTY_SESSION_INTERRUPT_BYTE;
			session->input_count++;
			session_unlock(session);
			if (notify_space && !notify_input_space(session))
				fail_session(session, "input availability notification failed");
			service_pending_reads(session);
			continue;
		}
		reject_pending_reads(session);
		if (session->foreground_process_cap == CAP_ID_INVALID) {
			session->foreground_interrupt_pending = true;
		}
		else if (session->busy_process_cap == CAP_ID_INVALID) {
			process_cap               = session->foreground_process_cap;
			session->busy_process_cap = process_cap;
		}
		else if (session->busy_process_cap != session->foreground_process_cap) {
			session->foreground_interrupt_pending = true;
		}
		session_unlock(session);
		if (notify_space && !notify_input_space(session))
			fail_session(session, "input availability notification failed");
		if (process_cap == CAP_ID_INVALID) continue;
		syscall_status_t status = interrupt_process(process_cap);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE &&
		    status != SYSCALL_STATUS_BAD_ARGUMENT)
			printf("tty session: foreground interrupt failed: %u\n", (unsigned)status);
		finish_process_interrupt(session, process_cap);
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
		.line   = terminal_cursor.line,
	};
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

/* Forward a terminal-selection query through the session. */
static bool dispatch_get_selection(struct tty_session* session, const struct cap_request* call) {
	struct terminal_get_selection_response    terminal_selection;
	struct tty_session_get_selection_response response;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_get_selection_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status = terminal_get_selection(session->terminal_cap, &terminal_selection);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response = (struct tty_session_get_selection_response){
		.anchor = {.line = terminal_selection.anchor.line, .column = terminal_selection.anchor.column},
		.focus  = { .line = terminal_selection.focus.line,  .column = terminal_selection.focus.column},
	};
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

/* Forward a controlling shell's selection replacement to the terminal. */
static bool dispatch_set_selection(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct tty_session_set_selection_request request;
	struct terminal_selection_position       anchor;
	struct terminal_selection_position       focus;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.anchor.line == 0u || request.focus.line == 0u ||
	    (request.anchor.line == request.focus.line && request.anchor.column == request.focus.column))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	anchor = (struct terminal_selection_position){.line = request.anchor.line, .column = request.anchor.column};
	focus  = (struct terminal_selection_position){.line = request.focus.line, .column = request.focus.column};
	return reply(call, NULL, 0u, terminal_set_selection(session->terminal_cap, anchor, focus));
}

/* Forward a controlling shell's selection clear to the terminal. */
static bool dispatch_clear_selection(struct tty_session* session, const struct cap_request* call) {
	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_clear_selection_request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply(call, NULL, 0u, terminal_clear_selection(session->terminal_cap));
}

/* Forward a clipboard query through the session. */
static bool dispatch_get_clipboard(struct tty_session* session, const struct cap_request* call) {
	uint8_t                                    response_data[CAP_MAX_RESPONSE_SIZE];
	struct tty_session_get_clipboard_response* response = (void*)response_data;
	size_t                                     size;
	size_t                                     response_size;

	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_get_clipboard_request))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status =
		terminal_get_clipboard(session->terminal_cap, response->text, TTY_SESSION_CLIPBOARD_CAPACITY, &size);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	response_size = sizeof(*response) + size;
	if (call->response_capacity < response_size) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	response->size     = (uint32_t)size;
	response->reserved = 0u;
	return reply(call, response, response_size, SYSCALL_STATUS_OK);
}

/* Forward a controlling shell's clipboard replacement to the terminal. */
static bool dispatch_set_clipboard(struct tty_session* session, const struct cap_request* call, const void* data) {
	struct tty_session_set_clipboard_request request;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size < sizeof(request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved != 0u || request.size > TTY_SESSION_CLIPBOARD_CAPACITY ||
	    call->request_size != sizeof(request) + request.size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply(call,
	             NULL,
	             0u,
	             terminal_set_clipboard(session->terminal_cap, (const uint8_t*)data + sizeof(request), request.size));
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
	session_unlock(session);
	if (!pause_and_discard_input(session)) return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);

	session_lock(session);
	bool success = reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (success) {
		reject_pending_reads(session);
		(void)discard_input(session);
		session->foreground_pid               = request.process_id;
		session->foreground_interrupt_pending = false;
	}
	session->input_paused = false;
	session_unlock(session);
	return success && notify_input_space(session);
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
		session->foreground_process_cap = request.process_cap;
		interrupt_pending = session->foreground_interrupt_pending && session->busy_process_cap == CAP_ID_INVALID;
		if (interrupt_pending) {
			session->busy_process_cap             = request.process_cap;
			session->foreground_interrupt_pending = false;
		}
	}
	session_unlock(session);
	if (success && interrupt_pending) {
		syscall_status_t status = interrupt_process(request.process_cap);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_UNAVAILABLE &&
		    status != SYSCALL_STATUS_BAD_ARGUMENT)
			printf("tty session: foreground interrupt failed: %u\n", (unsigned)status);
		finish_process_interrupt(session, request.process_cap);
	}
	return success;
}

/* Return foreground ownership to the controlling shell. */
static bool dispatch_restore_foreground(struct tty_session* session, const struct cap_request* call) {
	cap_id_t process_cap = CAP_ID_INVALID;

	if ((call->rights & CAP_MANAGE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct tty_session_restore_foreground_request) || call->response_capacity != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!pause_and_discard_input(session)) return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);

	session_lock(session);
	bool success = reply(call, NULL, 0u, SYSCALL_STATUS_OK);
	if (success) {
		reject_pending_reads(session);
		(void)discard_input(session);
		process_cap                           = session->foreground_process_cap;
		session->foreground_process_cap       = CAP_ID_INVALID;
		session->foreground_pid               = PROCESS_PID_INVALID;
		session->foreground_interrupt_pending = false;
		if (session->busy_process_cap == process_cap) {
			session->retired_process_cap = process_cap;
			process_cap                  = CAP_ID_INVALID;
		}
	}
	session->input_paused = false;
	session_unlock(session);
	if (success && !drop_process_capability(process_cap))
		printf("tty session: foreground process capability drop failed\n");
	return success && notify_input_space(session);
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
	case TTY_SESSION_OP_GET_SELECTION:
		return dispatch_get_selection(session, call);
	case TTY_SESSION_OP_GET_CLIPBOARD:
		return dispatch_get_clipboard(session, call);
	case TTY_SESSION_OP_SET_CLIPBOARD:
		return dispatch_set_clipboard(session, call, data);
	case TTY_SESSION_OP_BEGIN_FOREGROUND:
		return dispatch_begin_foreground(session, call, data);
	case TTY_SESSION_OP_ATTACH_FOREGROUND:
		return dispatch_attach_foreground(session, call, data);
	case TTY_SESSION_OP_RESTORE_FOREGROUND:
		return dispatch_restore_foreground(session, call);
	case TTY_SESSION_OP_SET_SELECTION:
		return dispatch_set_selection(session, call, data);
	case TTY_SESSION_OP_CLEAR_SELECTION:
		return dispatch_clear_selection(session, call);
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
		.interrupt_signal_cap   = CAP_ID_INVALID,
		.service_cap            = CAP_ID_INVALID,
		.session_pid            = PROCESS_PID_INVALID,
		.foreground_pid         = PROCESS_PID_INVALID,
		.foreground_process_cap = CAP_ID_INVALID,
		.retired_process_cap    = CAP_ID_INVALID,
		.busy_process_cap       = CAP_ID_INVALID,
		.input_epoch            = 1u,
	};
	struct self_info self;
	cap_id_t         input_thread_cap     = CAP_ID_INVALID;
	cap_id_t         interrupt_thread_cap = CAP_ID_INVALID;

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
	if (signal_create(&session.input_space_cap) != SYSCALL_STATUS_OK ||
	    terminal_acquire_interrupt_signal(session.terminal_cap, &session.interrupt_signal_cap) != SYSCALL_STATUS_OK ||
	    terminal_discard_input(session.terminal_cap, &session.interrupt_generation) != SYSCALL_STATUS_OK ||
	    !advertise_session(&session, self.pid)) {
		printf("tty session: advertisement failed\n");
		return 1;
	}
	struct tty_session* argument = &session;
	if (process_spawn_thread(
			session.self_cap, (uintptr_t)input_thread, &argument, sizeof(argument), NULL, 0u, &input_thread_cap) !=
	        SYSCALL_STATUS_OK ||
	    thread_detach(input_thread_cap) != SYSCALL_STATUS_OK ||
	    process_spawn_thread(session.self_cap,
	                         (uintptr_t)interrupt_thread,
	                         &argument,
	                         sizeof(argument),
	                         NULL,
	                         0u,
	                         &interrupt_thread_cap) != SYSCALL_STATUS_OK ||
	    thread_detach(interrupt_thread_cap) != SYSCALL_STATUS_OK) {
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
