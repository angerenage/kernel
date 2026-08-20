#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/time.h>

#include "launcher.h"
#include "registry.h"

static channel_id_t server_endpoint = CHANNEL_ID_INVALID;
static process_id_t server_pid      = PROCESS_PID_INVALID;

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size,
                          syscall_status_t response_status) {
	syscall_status_t status = channel_reply(call_id, response, response_size, response_status);
	if (status == SYSCALL_STATUS_OK) return true;
	printf("init: channel reply failed: %u\n", (unsigned)status);
	return false;
}

bool server_init(void) {
	struct self_info self;
	syscall_status_t status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) {
		printf("init: self process query failed: %u\n", (unsigned)status);
		return false;
	}
	status = channel_create(&server_endpoint);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: service channel creation failed: %u\n", (unsigned)status);
		return false;
	}
	server_pid = self.pid;
	return true;
}

void server_deinit(void) {
	if (server_endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(server_endpoint);
	server_endpoint = CHANNEL_ID_INVALID;
	server_pid      = PROCESS_PID_INVALID;
}

syscall_status_t init_server_grant(process_id_t target, cap_id_t* out_cap) {
	if (server_endpoint == CHANNEL_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return cap_publish(
		server_endpoint, INIT_SERVICE_OBJECT_ID, target, CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER, out_cap);
}

static bool dispatch_request(const struct cap_request* request, const void* data) {
	const struct init_request_header* header = data;

	switch (header->op) {
	case INIT_OP_GET_INFO: {
		const struct init_get_info_response response = {
			.status   = INIT_REGISTRY_OK,
			.init_pid = server_pid,
		};
		if (request->request_size != sizeof(struct init_get_info_request) ||
		    request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_ADVERTISE: {
		const struct init_advertise_request* advertise = data;
		struct init_registry_response        response;
		if (request->request_size != sizeof(*advertise) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_advertise(
			request->caller, &advertise->selector, advertise->minor, advertise->capability, advertise->client_rights);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_WITHDRAW: {
		const struct init_withdraw_request* withdraw = data;
		struct init_registry_response       response;
		if (request->request_size != sizeof(*withdraw) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_withdraw(request->caller, &withdraw->selector);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_ACQUIRE: {
		const struct init_acquire_request* acquire  = data;
		struct init_acquire_response       response = {0};
		if (request->request_size != sizeof(*acquire) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_acquire(request->caller, &acquire->query, acquire->service, &response.handle);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_ENUMERATE: {
		const struct init_enumerate_request* enumerate = data;
		struct init_enumerate_response*      response;
		size_t                               capacity;
		size_t                               response_size;
		if (request->request_size != sizeof(*enumerate) || request->response_capacity < sizeof(*response) ||
		    enumerate->size > (request->response_capacity - sizeof(*response)) / sizeof(struct init_service_info))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)enumerate->size * sizeof(struct init_service_info);
		response = malloc(capacity);
		if (response == NULL) {
			const struct init_enumerate_response failure = {
				.status = INIT_REGISTRY_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_enumerate(&enumerate->query,
                                              enumerate->offset,
                                              enumerate->size,
                                              response->entries,
                                              &response->returned,
                                              &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct init_service_info);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case INIT_OP_BROWSE: {
		const struct init_browse_request* browse = data;
		struct init_browse_response*      response;
		size_t                            capacity;
		size_t                            response_size;
		if (request->request_size != sizeof(*browse) || request->response_capacity < sizeof(*response) ||
		    browse->size > (request->response_capacity - sizeof(*response)) / sizeof(struct init_browse_entry))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)browse->size * sizeof(struct init_browse_entry);
		response = malloc(capacity);
		if (response == NULL) {
			const struct init_browse_response failure = {
				.status = INIT_REGISTRY_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_browse(browse->namespace_path,
                                           browse->offset,
                                           browse->size,
                                           response->entries,
                                           &response->returned,
                                           &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct init_browse_entry);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case INIT_OP_WATCH: {
		const struct init_watch_request* watch    = data;
		struct init_watch_response       response = {0};
		if (request->request_size != sizeof(*watch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_watch(
			request->caller, &watch->query, watch->signal_capability, &response.subscription_id, &response.counter);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_UNWATCH: {
		const struct init_unwatch_request* unwatch = data;
		struct init_registry_response      response;
		if (request->request_size != sizeof(*unwatch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_unwatch(request->caller, unwatch->subscription_id);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	default:
		return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

int server_run(const struct init_startup_info* startup) {
	union {
		struct init_request_header    header;
		struct init_advertise_request advertise;
		struct init_withdraw_request  withdraw;
		struct init_acquire_request   acquire;
		struct init_enumerate_request enumerate;
		struct init_browse_request    browse;
		struct init_watch_request     watch;
		struct init_unwatch_request   unwatch;
	} buffer;
	struct cap_request request;
	bool               received;
	bool               loader_started = false;
	syscall_status_t   status;

	if (server_endpoint == CHANNEL_ID_INVALID || startup == NULL) return 1;
	for (;;) {
		received = false;
		status   = channel_recv(server_endpoint, &request, &buffer, sizeof(buffer), &received);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: channel receive failed: %u\n", (unsigned)status);
			return 1;
		}
		if (!received) {
			if (!loader_started) {
				if (!loader_launch(startup)) return 1;
				loader_started = true;
				continue;
			}
			(void)sched_yield();
			continue;
		}
		if (request.object_id != INIT_SERVICE_OBJECT_ID || (request.rights & CAP_CALL) == 0u ||
		    request.request_size < sizeof(buffer.header)) {
			if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
			continue;
		}
		if (!dispatch_request(&request, &buffer)) return 1;
	}
}
