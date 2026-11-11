#include "pipe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <base/cap.h>

#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <runtime/stream.h>

enum { PIPE_BUFFER_SIZE = 16384u, PIPE_CONTROL_OBJECT = UINT64_MAX };

enum {
	PIPE_CONTROL_CREATE = 1u,
	PIPE_CONTROL_DESTROY = 2u,
	PIPE_CONTROL_CLOSE_READ = 3u,
	PIPE_CONTROL_CLOSE_WRITE = 4u,
};

struct pipe_control_request {
	uint32_t op;
	uint32_t count;
};

struct pipe_control_response {
	struct shell_pipe_pair pairs[SHELL_PIPE_MAX];
};

struct pending_read {
	cap_call_id_t call;
	size_t requested;
};

struct pending_write {
	cap_call_id_t call;
	size_t size;
	uint8_t data[CAP_MAX_REQUEST_SIZE];
};

struct pipe_state {
	uint8_t buffer[PIPE_BUFFER_SIZE];
	size_t head;
	size_t size;
	bool reader_open;
	bool writer_open;
	struct pending_read reader;
	struct pending_write writer;
};

struct pipe_server {
	channel_id_t endpoint;
	cap_id_t activity;
	cap_id_t control;
	process_id_t owner;
	struct pipe_state* pipes;
	size_t count;
	uint64_t base; /* Unique object IDs prevent stale EOF events. */
	uint64_t next_base;
	bool started;
};

static struct pipe_server server = {
	.endpoint = CHANNEL_ID_INVALID,
	.activity = CAP_ID_INVALID,
	.control = CAP_ID_INVALID,
};

static bool reply(cap_call_id_t call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call, data, size, status) == SYSCALL_STATUS_OK;
}

static size_t fifo_read(struct pipe_state* pipe, uint8_t* output, size_t maximum) {
	size_t n = pipe->size < maximum ? pipe->size : maximum;
	for (size_t i = 0u; i < n; i++) output[i] = pipe->buffer[(pipe->head + i) % PIPE_BUFFER_SIZE];
	pipe->head = (pipe->head + n) % PIPE_BUFFER_SIZE;
	pipe->size -= n;
	return n;
}

static size_t fifo_write(struct pipe_state* pipe, const uint8_t* input, size_t maximum) {
	size_t free_size = PIPE_BUFFER_SIZE - pipe->size;
	size_t n = maximum < free_size ? maximum : free_size;
	for (size_t i = 0u; i < n; i++) pipe->buffer[(pipe->head + pipe->size + i) % PIPE_BUFFER_SIZE] = input[i];
	pipe->size += n;
	return n;
}

static void serve_pending(struct pipe_state* pipe) {
	uint8_t data[CAP_MAX_RESPONSE_SIZE];
	for (;;) {
		bool progress = false;
		if (pipe->reader.call != CAP_CALL_ID_INVALID &&
				(pipe->size != 0u || !__atomic_load_n(&pipe->writer_open, __ATOMIC_ACQUIRE))) {
			size_t n = fifo_read(pipe, data, pipe->reader.requested);
			(void)reply(pipe->reader.call, n != 0u ? data : NULL, n, SYSCALL_STATUS_OK);
			pipe->reader.call = CAP_CALL_ID_INVALID;
			progress = true;
		}
		if (pipe->writer.call != CAP_CALL_ID_INVALID &&
				(!__atomic_load_n(&pipe->reader_open, __ATOMIC_ACQUIRE) || pipe->size < PIPE_BUFFER_SIZE)) {
			if (!__atomic_load_n(&pipe->reader_open, __ATOMIC_ACQUIRE)) {
				(void)reply(pipe->writer.call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
			}
			else {
				size_t n = fifo_write(pipe, pipe->writer.data, pipe->writer.size);
				struct stream_write_response response = {.size = n};
				(void)reply(pipe->writer.call, &response, sizeof(response), SYSCALL_STATUS_OK);
			}
			pipe->writer.call = CAP_CALL_ID_INVALID;
			progress = true;
		}
		if (!progress) break;
	}
}

static void handle_stream(const struct cap_request* call, const uint8_t* request) {
	uint64_t object = call->object_id;
	if (object < server.base || object - server.base >= server.count * 2u ||
			call->request_size < sizeof(struct stream_request_header)) {
		(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return;
	}
	struct pipe_state* pipe = &server.pipes[(object - server.base) / 2u];
	bool read_endpoint = ((object - server.base) & 1u) == 0u;
	struct stream_request_header header;
	memcpy(&header, request, sizeof(header));
	if (read_endpoint && header.op == STREAM_OP_READ) {
		struct stream_read_request in;
		if ((call->rights & (CAP_CALL | CAP_READ)) != (CAP_CALL | CAP_READ) || call->request_size != sizeof(in))
			goto bad_argument;
		memcpy(&in, request, sizeof(in));
		if (in.reserved != 0u || in.size > CAP_MAX_RESPONSE_SIZE || in.size > call->response_capacity ||
				pipe->reader.call != CAP_CALL_ID_INVALID)
			goto bad_argument;
		if (in.size == 0u) {
			(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_OK);
			return;
		}
		pipe->reader = (struct pending_read){.call = call->call_id, .requested = (size_t)in.size};
		serve_pending(pipe);
		return;
	}
	if (!read_endpoint && header.op == STREAM_OP_WRITE) {
		struct stream_write_request in;
		if ((call->rights & (CAP_CALL | CAP_WRITE)) != (CAP_CALL | CAP_WRITE) || call->request_size < sizeof(in) ||
				pipe->writer.call != CAP_CALL_ID_INVALID ||
				call->response_capacity < sizeof(struct stream_write_response))
			goto bad_argument;
		memcpy(&in, request, sizeof(in));
		if (in.reserved != 0u || in.size != call->request_size - sizeof(in) || in.size > sizeof(pipe->writer.data))
			goto bad_argument;
		if (in.size == 0u) {
			const struct stream_write_response response = {.size = 0u};
			(void)reply(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
			return;
		}
		pipe->writer.call = call->call_id;
		pipe->writer.size = (size_t)in.size;
		memcpy(pipe->writer.data, request + sizeof(in), (size_t)in.size);
		serve_pending(pipe);
		return;
	}
bad_argument:
	(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
}

static void handle_lifecycle(const struct channel_event* event) {
	if (event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->object_id < server.base ||
			event->object_id - server.base >= server.count * 2u)
		return;
	struct pipe_state* pipe = &server.pipes[(event->object_id - server.base) / 2u];
	if (((event->object_id - server.base) & 1u) == 0u) __atomic_store_n(&pipe->reader_open, false, __ATOMIC_RELEASE);
	else __atomic_store_n(&pipe->writer_open, false, __ATOMIC_RELEASE);
	serve_pending(pipe);
}

static void release_pipe_objects(void) {
	for (size_t i = 0u; i < server.count; i++) {
		struct pipe_state* pipe = &server.pipes[i];
		if (pipe->reader.call != CAP_CALL_ID_INVALID)
			(void)reply(pipe->reader.call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
		if (pipe->writer.call != CAP_CALL_ID_INVALID)
			(void)reply(pipe->writer.call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
		(void)cap_unpublish(server.endpoint, server.base + i * 2u);
		(void)cap_unpublish(server.endpoint, server.base + i * 2u + 1u);
	}
	free(server.pipes);
	__atomic_store_n(&server.pipes, NULL, __ATOMIC_RELEASE);
	server.count = 0u;
}

static void handle_control(const struct cap_request* call, const uint8_t* request) {
	struct pipe_control_request in;
	if (call->request_size != sizeof(in) || (call->rights & CAP_CALL) == 0u) goto bad_argument;
	memcpy(&in, request, sizeof(in));
	if (in.op == PIPE_CONTROL_CLOSE_READ || in.op == PIPE_CONTROL_CLOSE_WRITE) {
		if (in.count >= server.count || server.pipes == NULL) goto bad_argument;
		struct pipe_state* pipe = &server.pipes[in.count];
		if (in.op == PIPE_CONTROL_CLOSE_READ) __atomic_store_n(&pipe->reader_open, false, __ATOMIC_RELEASE);
		else __atomic_store_n(&pipe->writer_open, false, __ATOMIC_RELEASE);
		serve_pending(pipe);
		(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_OK);
		return;
	}
	if (in.op == PIPE_CONTROL_DESTROY) {
		if (server.count != 0u) release_pipe_objects();
		(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_OK);
		return;
	}
	if (in.op != PIPE_CONTROL_CREATE || server.count != 0u || in.count == 0u || in.count > SHELL_PIPE_MAX ||
			server.next_base > UINT64_MAX - SHELL_PIPE_MAX * 2u - 1u ||
			call->response_capacity < sizeof(struct pipe_control_response))
		goto bad_argument;
	struct pipe_control_response response = {0};
	struct pipe_state* pipes = calloc(in.count, sizeof(*pipes));
	if (pipes == NULL) {
		(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_FAILED);
		return;
	}
	__atomic_store_n(&server.pipes, pipes, __ATOMIC_RELEASE);
	server.count = in.count;
	server.base = server.next_base;
	server.next_base += SHELL_PIPE_MAX * 2u;
	for (size_t i = 0u; i < in.count; i++) {
		struct pipe_state* pipe = &pipes[i];
		pipe->reader_open = true;
		pipe->writer_open = true;
		syscall_status_t status = cap_publish(server.endpoint,
				server.base + i * 2u,
				server.owner,
				CAP_CALL | CAP_READ | CAP_DELEGATE,
				&response.pairs[i].read_cap);
		if (status == SYSCALL_STATUS_OK)
			status = cap_publish(server.endpoint,
					server.base + i * 2u + 1u,
					server.owner,
					CAP_CALL | CAP_WRITE | CAP_DELEGATE,
					&response.pairs[i].write_cap);
		if (status != SYSCALL_STATUS_OK) {
			for (size_t j = 0u; j <= i; j++) {
				if (response.pairs[j].read_cap != CAP_ID_INVALID) (void)cap_drop(response.pairs[j].read_cap);
				if (response.pairs[j].write_cap != CAP_ID_INVALID) (void)cap_drop(response.pairs[j].write_cap);
			}
			release_pipe_objects();
			(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_FAILED);
			return;
		}
	}
	(void)reply(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	return;
bad_argument:
	(void)reply(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
}

static void pipe_worker(void* argument) {
	static uint8_t request_bytes[CAP_MAX_REQUEST_SIZE];
	(void)argument;
	for (;;) {
		bool received;
		do {
			struct cap_request request;
			received = false;
			if (channel_recv(server.endpoint, &request, request_bytes, sizeof(request_bytes), &received) !=
					SYSCALL_STATUS_OK)
				return;
			if (received) {
				if (request.object_id == PIPE_CONTROL_OBJECT) handle_control(&request, request_bytes);
				else handle_stream(&request, request_bytes);
			}
		} while (received);
		do {
			struct channel_event event;
			received = false;
			if (channel_event_recv(server.endpoint, &event, &received) != SYSCALL_STATUS_OK) return;
			if (received) handle_lifecycle(&event);
		} while (received);
		struct signal_message signal;
		if (signal_wait(server.activity, &signal) != SYSCALL_STATUS_OK) return;
	}
}

static bool ensure_server(void) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID
	};
	if (server.started) return true;
	if (process_self_info(&self) != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID ||
			self.self_cap == CAP_ID_INVALID)
		goto fail;
	if (channel_create(&server.endpoint, &server.activity) != SYSCALL_STATUS_OK) goto fail;
	server.owner = self.pid;
	server.next_base = 1u;
	if (cap_publish(server.endpoint, PIPE_CONTROL_OBJECT, self.pid, CAP_CALL, &server.control) != SYSCALL_STATUS_OK)
		goto fail;
	if (process_spawn_detached_thread(self.self_cap, (uintptr_t)pipe_worker, NULL, 0u, NULL, 0u) != SYSCALL_STATUS_OK)
		goto fail;
	server.started = true;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	return true;
fail:
	if (server.control != CAP_ID_INVALID) (void)cap_drop(server.control);
	server.control = CAP_ID_INVALID;
	if (server.endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(server.endpoint);
	server.endpoint = CHANNEL_ID_INVALID;
	server.activity = CAP_ID_INVALID;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	return false;
}

bool shell_pipe_begin(size_t count, struct shell_pipe_pair pairs[SHELL_PIPE_MAX]) {
	struct pipe_control_request request = {.op = PIPE_CONTROL_CREATE, .count = (uint32_t)count};
	struct pipe_control_response response;
	size_t response_size = 0u;
	if (pairs == NULL || count == 0u || count > SHELL_PIPE_MAX || !ensure_server()) return false;
	if (cap_call(server.control, &request, sizeof(request), &response, sizeof(response), &response_size) !=
					SYSCALL_STATUS_OK ||
			response_size != sizeof(response))
		return false;
	memcpy(pairs, response.pairs, count * sizeof(*pairs));
	return true;
}

void shell_pipe_drop(cap_id_t* cap) {
	if (cap == NULL || *cap == CAP_ID_INVALID) return;
	(void)cap_drop(*cap);
	*cap = CAP_ID_INVALID;
}

bool shell_pipe_finish(void) {
	struct pipe_control_request request = {.op = PIPE_CONTROL_DESTROY};
	size_t response_size = 0u;
	if (!server.started) return true;
	return cap_call(server.control, &request, sizeof(request), NULL, 0u, &response_size) == SYSCALL_STATUS_OK &&
	       response_size == 0u;
}

static bool close_end(uint32_t op, size_t index) {
	struct pipe_control_request request = {.op = op, .count = (uint32_t)index};
	size_t response_size = 0u;
	return server.started && index < server.count &&
	       cap_call(server.control, &request, sizeof(request), NULL, 0u, &response_size) == SYSCALL_STATUS_OK &&
	       response_size == 0u;
}

bool shell_pipe_close_reader(size_t index) {
	return close_end(PIPE_CONTROL_CLOSE_READ, index);
}

bool shell_pipe_close_writer(size_t index) {
	return close_end(PIPE_CONTROL_CLOSE_WRITE, index);
}
