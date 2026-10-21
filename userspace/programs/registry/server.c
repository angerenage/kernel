#include "server.h"

#include <runtime/registry.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#include "registry.h"

static channel_id_t registry_endpoint = CHANNEL_ID_INVALID;
static process_id_t registry_pid      = PROCESS_PID_INVALID;

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size,
                          syscall_status_t response_status) {
	syscall_status_t status = channel_reply(call_id, response, response_size, response_status);
	if (status == SYSCALL_STATUS_OK) return true;
	printf("registry: channel reply failed: %u\n", (unsigned)status);
	return false;
}

static bool dispatch_request(const struct cap_request* request, const void* data) {
	const struct registry_request_header* header = data;

	switch (header->op) {
	case REGISTRY_OP_GET_INFO: {
		const struct registry_get_info_response response = {
			.status       = REGISTRY_STATUS_OK,
			.registry_pid = registry_pid,
		};
		if (request->request_size != sizeof(struct registry_get_info_request) ||
		    request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case REGISTRY_OP_ADVERTISE: {
		const struct registry_advertise_request* advertise = data;
		struct registry_response                 response;
		if (request->request_size != sizeof(*advertise) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_state_advertise(
			request->caller, &advertise->selector, advertise->minor, advertise->capability, advertise->client_rights);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case REGISTRY_OP_WITHDRAW: {
		const struct registry_withdraw_request* withdraw = data;
		struct registry_response                response;
		if (request->request_size != sizeof(*withdraw) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_state_withdraw(request->caller, &withdraw->selector);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case REGISTRY_OP_ACQUIRE: {
		const struct registry_acquire_request* acquire  = data;
		struct registry_acquire_response       response = {0};
		bool                                   replied;
		if (request->request_size != sizeof(*acquire) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_state_acquire(request->caller, &acquire->query, acquire->service, &response.handle);
		replied         = reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
		if (!replied && response.status == REGISTRY_STATUS_OK && response.handle.capability != CAP_ID_INVALID)
			(void)cap_revoke(response.handle.capability, 0u);
		return replied;
	}
	case REGISTRY_OP_ENUMERATE: {
		const struct registry_enumerate_request* enumerate = data;
		struct registry_enumerate_response*      response;
		size_t                                   capacity;
		size_t                                   response_size;
		if (request->request_size != sizeof(*enumerate) || request->response_capacity < sizeof(*response) ||
		    enumerate->size > (request->response_capacity - sizeof(*response)) / sizeof(struct registry_service_info))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)enumerate->size * sizeof(struct registry_service_info);
		response = malloc(capacity);
		if (response == NULL) {
			const struct registry_enumerate_response failure = {
				.status = REGISTRY_STATUS_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_state_enumerate(&enumerate->query,
		                                              enumerate->offset,
		                                              enumerate->size,
		                                              response->entries,
		                                              &response->returned,
		                                              &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct registry_service_info);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case REGISTRY_OP_BROWSE: {
		const struct registry_browse_request* browse = data;
		struct registry_browse_response*      response;
		size_t                                capacity;
		size_t                                response_size;
		if (request->request_size != sizeof(*browse) || request->response_capacity < sizeof(*response) ||
		    browse->size > (request->response_capacity - sizeof(*response)) / sizeof(struct registry_browse_entry))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)browse->size * sizeof(struct registry_browse_entry);
		response = malloc(capacity);
		if (response == NULL) {
			const struct registry_browse_response failure = {
				.status = REGISTRY_STATUS_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_state_browse(browse->namespace_path,
		                                           browse->offset,
		                                           browse->size,
		                                           response->entries,
		                                           &response->returned,
		                                           &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct registry_browse_entry);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case REGISTRY_OP_WATCH: {
		const struct registry_watch_request* watch    = data;
		struct registry_watch_response       response = {0};
		if (request->request_size != sizeof(*watch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_state_watch(
			request->caller, &watch->query, watch->signal_capability, &response.subscription_id, &response.counter);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case REGISTRY_OP_UNWATCH: {
		const struct registry_unwatch_request* unwatch = data;
		struct registry_response               response;
		if (request->request_size != sizeof(*unwatch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_state_unwatch(request->caller, unwatch->subscription_id);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	default:
		return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

int registry_server_run(process_id_t init_pid, cap_id_t ready_cap) {
	union {
		struct registry_request_header    header;
		struct registry_advertise_request advertise;
		struct registry_withdraw_request  withdraw;
		struct registry_acquire_request   acquire;
		struct registry_enumerate_request enumerate;
		struct registry_browse_request    browse;
		struct registry_watch_request     watch;
		struct registry_unwatch_request   unwatch;
	} buffer;
	struct signal_send_response ready_response;
	struct cap_request          request;
	struct self_info            self;
	cap_id_t                    activity     = CAP_ID_INVALID;
	cap_id_t                    registry_cap = CAP_ID_INVALID;
	bool                        received;
	syscall_status_t            status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return 1;
	registry_pid = self.pid;
	status       = channel_create(&registry_endpoint, &activity);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(registry_endpoint,
		                     REGISTRY_SERVICE_OBJECT_ID,
		                     init_pid,
		                     CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER,
		                     &registry_cap);
	if (status == SYSCALL_STATUS_OK)
		status = signal_send(ready_cap, registry_cap, 0u, 0u, 0u, SIGNAL_SEND_FLAG_NONE, &ready_response);
	if (status != SYSCALL_STATUS_OK) return 1;
	(void)cap_drop(ready_cap);

	for (;;) {
		do {
			received = false;
			status   = channel_recv(registry_endpoint, &request, &buffer, sizeof(buffer), &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received) {
				if (request.object_id != REGISTRY_SERVICE_OBJECT_ID || (request.rights & CAP_CALL) == 0u ||
				    request.request_size < sizeof(buffer.header)) {
					if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				}
				else if (!dispatch_request(&request, &buffer)) return 1;
			}
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(registry_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
		} while (received);
		struct signal_message message;
		status = signal_wait(activity, &message);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}
