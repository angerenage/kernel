#include "foreground_input.h"

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <runtime/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/display.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>

#include "shell.h"

#define SHELL_STDIN_OBJECT_ID 1u
#define SHELL_STDIN_PENDING_CAPACITY 64u

struct pending_read {
	cap_call_id_t call_id;
	size_t        size;
};

struct shell_foreground_input {
	struct shell*       shell;
	cap_id_t            process_cap;
	channel_id_t        endpoint;
	cap_id_t            activity_cap;
	cap_id_t            source_cap;
	cap_id_t            reader_thread_cap;
	cap_id_t            server_thread_cap;
	struct pending_read pending[SHELL_STDIN_PENDING_CAPACITY];
	size_t              pending_head;
	size_t              pending_count;
	bool                locked;
};

struct foreground_thread_argument {
	struct shell_foreground_input* input;
};

static void input_lock(struct shell_foreground_input* input) {
	while (__atomic_test_and_set(&input->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void input_unlock(struct shell_foreground_input* input) {
	__atomic_clear(&input->locked, __ATOMIC_RELEASE);
}

static void consume_bytes(struct shell* shell, size_t count) {
	shell->pending_input_head = (shell->pending_input_head + count) % SHELL_PENDING_INPUT_CAPACITY;
	shell->pending_input_count -= count;
}

static size_t copy_bytes(const struct shell* shell, uint8_t* output, size_t capacity) {
	size_t count = shell->pending_input_count < capacity ? shell->pending_input_count : capacity;
	for (size_t i = 0u; i < count; i++)
		output[i] = shell->pending_input[(shell->pending_input_head + i) % SHELL_PENDING_INPUT_CAPACITY];
	return count;
}

static void service_pending_reads(struct shell_foreground_input* input) {
	for (;;) {
		uint8_t       response[CAP_MAX_RESPONSE_SIZE];
		cap_call_id_t call_id;
		size_t        count;

		input_lock(input);
		if (input->pending_count == 0u || input->shell->pending_input_count == 0u) {
			input_unlock(input);
			return;
		}
		struct pending_read* pending = &input->pending[input->pending_head];
		call_id                      = pending->call_id;
		count                        = copy_bytes(input->shell, response, pending->size);
		consume_bytes(input->shell, count);
		input->pending_head = (input->pending_head + 1u) % SHELL_STDIN_PENDING_CAPACITY;
		input->pending_count--;
		input_unlock(input);
		(void)channel_reply(call_id, response, count, SYSCALL_STATUS_OK);
	}
}

static bool enqueue_byte(struct shell_foreground_input* input, uint8_t byte) {
	bool success = false;

	input_lock(input);
	if (input->shell->pending_input_count < SHELL_PENDING_INPUT_CAPACITY) {
		size_t tail =
			(input->shell->pending_input_head + input->shell->pending_input_count) % SHELL_PENDING_INPUT_CAPACITY;
		input->shell->pending_input[tail] = byte;
		input->shell->pending_input_count++;
		success = true;
	}
	input_unlock(input);
	if (success) service_pending_reads(input);
	return success;
}

static bool reply(const struct cap_request* call, syscall_status_t status) {
	return channel_reply(call->call_id, NULL, 0u, status) == SYSCALL_STATUS_OK;
}

static void dispatch_read(struct shell_foreground_input* input, const struct cap_request* call, const uint8_t* data) {
	struct stream_read_request request;

	if (call->object_id != SHELL_STDIN_OBJECT_ID || data == NULL || call->request_size != sizeof(request)) {
		(void)reply(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return;
	}
	memcpy(&request, data, sizeof(request));
	if ((call->rights & CAP_READ) == 0u) {
		(void)reply(call, SYSCALL_STATUS_DENIED);
		return;
	}
	if (request.header.op != STREAM_OP_READ || request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE ||
	    request.size > call->response_capacity) {
		(void)reply(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return;
	}
	if (request.size == 0u) {
		(void)reply(call, SYSCALL_STATUS_OK);
		return;
	}

	input_lock(input);
	if (input->pending_count == SHELL_STDIN_PENDING_CAPACITY) {
		input_unlock(input);
		(void)reply(call, SYSCALL_STATUS_FAILED);
		return;
	}
	size_t tail          = (input->pending_head + input->pending_count) % SHELL_STDIN_PENDING_CAPACITY;
	input->pending[tail] = (struct pending_read){.call_id = call->call_id, .size = (size_t)request.size};
	input->pending_count++;
	input_unlock(input);
	service_pending_reads(input);
}

static void fail_foreground(struct shell_foreground_input* input) {
	(void)process_kill(input->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
}

static void reader_thread(void* opaque) {
	const struct foreground_thread_argument* argument = opaque;
	struct shell_foreground_input*           input    = argument->input;

	for (;;) {
		uint8_t byte;
		size_t  size = 0u;
		if (display_read(&byte, 1u, &size) != SYSCALL_STATUS_OK || size != 1u) {
			fail_foreground(input);
			thread_exit(1u);
		}
		if (byte == 0x03u) {
			syscall_status_t status = process_kill(input->process_cap, PROCESS_EXIT_SYSTEM_INTERRUPTED);
			thread_exit(status == SYSCALL_STATUS_OK || status == SYSCALL_STATUS_UNAVAILABLE ? 0u : 1u);
		}
		if (!enqueue_byte(input, byte)) {
			fail_foreground(input);
			thread_exit(1u);
		}
	}
}

static void server_thread(void* opaque) {
	const struct foreground_thread_argument* argument = opaque;
	struct shell_foreground_input*           input    = argument->input;
	uint8_t                                  request_data[CAP_MAX_REQUEST_SIZE];

	for (;;) {
		bool             received;
		syscall_status_t status;
		do {
			struct cap_request call;
			received = false;
			status   = channel_recv(input->endpoint, &call, request_data, sizeof(request_data), &received);
			if (status != SYSCALL_STATUS_OK) {
				fail_foreground(input);
				thread_exit(1u);
			}
			if (received) dispatch_read(input, &call, request_data);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(input->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) {
				fail_foreground(input);
				thread_exit(1u);
			}
		} while (received);
		struct signal_message activity;
		status = signal_wait(input->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK) {
			fail_foreground(input);
			thread_exit(1u);
		}
	}
}

static bool stop_thread(cap_id_t* thread_cap) {
	if (*thread_cap == CAP_ID_INVALID) return true;
	(void)thread_cancel(*thread_cap);
	bool success = thread_join(*thread_cap, NULL) == SYSCALL_STATUS_OK;
	*thread_cap  = CAP_ID_INVALID;
	return success;
}

struct shell_foreground_input* shell_foreground_input_create(struct shell* shell) {
	struct shell_foreground_input* input;
	syscall_status_t               status;

	if (shell == NULL || shell->self_cap == CAP_ID_INVALID || shell->pid == PROCESS_PID_INVALID) return NULL;
	input = calloc(1u, sizeof(*input));
	if (input == NULL) return NULL;
	*input = (struct shell_foreground_input){
		.shell             = shell,
		.process_cap       = CAP_ID_INVALID,
		.endpoint          = CHANNEL_ID_INVALID,
		.activity_cap      = CAP_ID_INVALID,
		.source_cap        = CAP_ID_INVALID,
		.reader_thread_cap = CAP_ID_INVALID,
		.server_thread_cap = CAP_ID_INVALID,
	};
	status = channel_create(&input->endpoint, &input->activity_cap);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(
			input->endpoint, SHELL_STDIN_OBJECT_ID, shell->pid, CAP_CALL | CAP_READ | CAP_DELEGATE, &input->source_cap);
	if (status != SYSCALL_STATUS_OK) {
		(void)shell_foreground_input_destroy(input);
		return NULL;
	}
	return input;
}

cap_id_t shell_foreground_input_capability(const struct shell_foreground_input* input) {
	return input == NULL ? CAP_ID_INVALID : input->source_cap;
}

bool shell_foreground_input_start(struct shell_foreground_input* input, cap_id_t process_cap) {
	const struct foreground_thread_argument argument = {.input = input};
	syscall_status_t                        status;

	if (input == NULL || process_cap == CAP_ID_INVALID || input->process_cap != CAP_ID_INVALID ||
	    input->reader_thread_cap != CAP_ID_INVALID || input->server_thread_cap != CAP_ID_INVALID)
		return false;
	input->process_cap = process_cap;
	status             = process_spawn_thread(input->shell->self_cap,
	                                          (uintptr_t)server_thread,
	                                          &argument,
	                                          sizeof(argument),
	                                          NULL,
	                                          0u,
	                                          &input->server_thread_cap);
	if (status == SYSCALL_STATUS_OK)
		status = process_spawn_thread(input->shell->self_cap,
		                              (uintptr_t)reader_thread,
		                              &argument,
		                              sizeof(argument),
		                              NULL,
		                              0u,
		                              &input->reader_thread_cap);
	if (status == SYSCALL_STATUS_OK) return true;
	(void)stop_thread(&input->reader_thread_cap);
	(void)stop_thread(&input->server_thread_cap);
	input->process_cap = CAP_ID_INVALID;
	return false;
}

bool shell_foreground_input_destroy(struct shell_foreground_input* input) {
	bool success;

	if (input == NULL) return true;
	success = stop_thread(&input->reader_thread_cap);
	if (!stop_thread(&input->server_thread_cap)) success = false;
	if (input->source_cap != CAP_ID_INVALID && cap_drop(input->source_cap) != SYSCALL_STATUS_OK) success = false;
	input->source_cap = CAP_ID_INVALID;
	if (input->activity_cap != CAP_ID_INVALID && cap_drop(input->activity_cap) != SYSCALL_STATUS_OK) success = false;
	input->activity_cap = CAP_ID_INVALID;
	if (input->endpoint != CHANNEL_ID_INVALID && channel_destroy(input->endpoint) != SYSCALL_STATUS_OK) success = false;
	input->endpoint = CHANNEL_ID_INVALID;
	free(input);
	return success;
}
