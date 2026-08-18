#include "server.h"

#include <base/cap.h>
#include <base/process.h>
#include <protocol/loader.h>
#include <runtime/init.h>
#include <runtime/program.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/time.h>

#include "load.h"

#define LOADER_SERVICE_NAME "elf64"
#define LOADER_PROTOCOL_VERSION_MAJOR 1u
#define LOADER_PROTOCOL_VERSION_MINOR 0u

#define LOADER_SERVICE_OBJECT_ID 1u

static channel_id_t                  loader_endpoint = CHANNEL_ID_INVALID;
static struct loader_loaded_program* loaded_programs;
static uint8_t                       request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static uint64_t loaded_object_id(const struct loader_loaded_program* program) {
	return (uint64_t)(uintptr_t)program;
}

static struct loader_loaded_program* find_loaded(uint64_t object_id) {
	for (struct loader_loaded_program* program = loaded_programs; program != NULL; program = program->next) {
		if (loaded_object_id(program) == object_id) return program;
	}
	return NULL;
}

static void unlink_loaded(struct loader_loaded_program* target) {
	struct loader_loaded_program** cursor = &loaded_programs;

	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor      = target->next;
			target->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static bool copy_request(const struct cap_request* call, const void* data, void* out, size_t size) {
	if (call == NULL || data == NULL || out == NULL || call->request_size < size) return false;
	memcpy(out, data, size);
	return true;
}

static bool handle_load(const struct cap_request* call, const void* data) {
	struct loader_v1_load_request  request;
	struct loader_loaded_program*  program = NULL;
	struct loader_v1_load_response response;
	cap_id_t                       load_cap = CAP_ID_INVALID;
	uint64_t                       expected_size;
	uint64_t                       object_id;
	syscall_status_t               status;

	if (!copy_request(call, data, &request, sizeof(request)))
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.reserved != 0u || request.name_size == 0u || request.blob_cap == CAP_ID_INVALID ||
	    request.name_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + (uint64_t)request.name_size) != call->request_size ||
	    call->response_capacity < sizeof(response))
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	status = loader_prepare_program(request.blob_cap, (const char*)data + sizeof(request), request.name_size, &program);
	if (status != SYSCALL_STATUS_OK) return reply_request(call->call_id, NULL, 0u, status);

	object_id = loaded_object_id(program);
	if (object_id == 0u || object_id == LOADER_SERVICE_OBJECT_ID) {
		loader_discard_program(program);
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_FAILED);
	}
	status = cap_publish(loader_endpoint, object_id, call->caller, LOADER_V1_LOAD_CAP_RIGHTS, &load_cap);
	if (status != SYSCALL_STATUS_OK) {
		loader_discard_program(program);
		return reply_request(call->call_id, NULL, 0u, status);
	}
	program->load_cap = load_cap;

	program->next   = loaded_programs;
	loaded_programs = program;
	response        = (struct loader_v1_load_response){
			   .load_cap   = load_cap,
			   .process_id = program->process_id,
    };
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	unlink_loaded(program);
	loader_discard_program(program);
	return false;
}

static bool handle_run(const struct cap_request* call, struct loader_loaded_program* program, const void* data) {
	struct loader_v1_run_request  request;
	struct loader_v1_run_response response = {
		.process_cap = CAP_ID_INVALID,
		.thread_cap  = CAP_ID_INVALID,
	};
	cap_id_t         loader_thread_cap = CAP_ID_INVALID;
	uint64_t         expected_size;
	syscall_status_t status;

	if (program == NULL || program->started || !copy_request(call, data, &request, sizeof(request)))
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.argv_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + (uint64_t)request.argv_size) != call->request_size ||
	    (request.argc == 0u) != (request.argv_size == 0u) || call->response_capacity < sizeof(response))
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	status = loader_start_program(program,
	                              request.argc,
	                              request.argv_size == 0u ? NULL : (const uint8_t*)data + sizeof(request),
	                              request.argv_size,
	                              &loader_thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		if (program->started) {
			unlink_loaded(program);
			loader_discard_program(program);
		}
		return reply_request(call->call_id, NULL, 0u, status);
	}

	status = cap_delegate(program->process_cap, call->caller, LOADER_V1_PROCESS_CAP_RIGHTS, &response.process_cap);
	if (status == SYSCALL_STATUS_OK) {
		status = cap_delegate(loader_thread_cap, call->caller, LOADER_V1_THREAD_CAP_RIGHTS, &response.thread_cap);
	}

	if (status != SYSCALL_STATUS_OK) {
		unlink_loaded(program);
		loader_discard_program(program);
		return reply_request(call->call_id, NULL, 0u, status);
	}
	status = cap_revoke(program->load_cap, 0u);
	if (status != SYSCALL_STATUS_OK) {
		unlink_loaded(program);
		loader_discard_program(program);
		return reply_request(call->call_id, NULL, 0u, status);
	}
	program->load_cap = CAP_ID_INVALID;
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	unlink_loaded(program);
	loader_discard_program(program);
	return false;
}

static bool handle_cancel(const struct cap_request* call, struct loader_loaded_program* program, const void* data) {
	struct loader_v1_cancel_request request;
	syscall_status_t                status;

	if (program == NULL || program->started || !copy_request(call, data, &request, sizeof(request)) ||
	    call->request_size != sizeof(request))
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	status = cap_revoke(program->load_cap, 0u);
	if (status != SYSCALL_STATUS_OK) return reply_request(call->call_id, NULL, 0u, status);
	program->load_cap = CAP_ID_INVALID;
	unlink_loaded(program);
	loader_discard_program(program);
	return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_OK);
}

static bool dispatch_request(const struct cap_request* call, const void* data) {
	struct loader_v1_request_header header;
	struct loader_loaded_program*   program;

	if (!copy_request(call, data, &header, sizeof(header)) || (call->rights & CAP_CALL) == 0u)
		return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->object_id == LOADER_SERVICE_OBJECT_ID) {
		if (header.op != LOADER_V1_OP_LOAD) return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return handle_load(call, data);
	}
	program = find_loaded(call->object_id);
	if (program == NULL) return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (header.op == LOADER_V1_OP_RUN) return handle_run(call, program, data);
	if (header.op == LOADER_V1_OP_CANCEL) return handle_cancel(call, program, data);
	return reply_request(call->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
}

static bool advertise_loader(void) {
	const struct init_service_selector selector = {
		.namespace_path = PROGRAM_LOADER_NAMESPACE,
		.protocol       = LOADER_PROTOCOL_NAME,
		.major          = LOADER_PROTOCOL_VERSION_MAJOR,
		.service        = LOADER_SERVICE_NAME,
	};
	struct self_info        self;
	struct init_call_result result;
	cap_id_t                service_cap = CAP_ID_INVALID;
	syscall_status_t        status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	status = channel_create(&loader_endpoint);
	if (status != SYSCALL_STATUS_OK) return false;
	status = cap_publish(loader_endpoint, LOADER_SERVICE_OBJECT_ID, self.pid, CAP_CALL | CAP_DELEGATE, &service_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	result = init_advertise(&selector, LOADER_PROTOCOL_VERSION_MINOR, service_cap, LOADER_V1_SERVICE_CAP_RIGHTS);
	return result.status == INIT_REGISTRY_OK && result.transport_status == SYSCALL_STATUS_OK;
}

int loader_server_run(void) {
	struct cap_request call;
	bool               received;
	syscall_status_t   status;

	if (!advertise_loader()) {
		printf("loader: advertisement failed\n");
		return 1;
	}
	printf("loader: advertised %s/%s\n", PROGRAM_LOADER_NAMESPACE, LOADER_SERVICE_NAME);

	for (;;) {
		received = false;
		status   = channel_recv(loader_endpoint, &call, request_buffer, sizeof(request_buffer), &received);
		if (status != SYSCALL_STATUS_OK) {
			printf("loader: channel receive failed: %u\n", (unsigned)status);
			return 1;
		}
		if (!received) {
			(void)sched_yield();
			continue;
		}
		if (!dispatch_request(&call, request_buffer)) printf("loader: channel reply failed\n");
	}
}
