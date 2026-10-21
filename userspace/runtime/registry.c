#include <base/cap.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <runtime/registry.h>
#include <stdbool.h>
#include <system/capability.h>

cap_id_t registry_cap_id = CAP_ID_INVALID;

static cap_id_t     cached_registry_cap = CAP_ID_INVALID;
static process_id_t cached_registry_pid = PROCESS_PID_INVALID;

static struct registry_call_result local_error(enum registry_status status, syscall_status_t transport) {
	return (struct registry_call_result){.status = status, .transport_status = transport};
}

static struct registry_call_result call_fixed(const void* request, size_t request_size, void* response,
                                              size_t response_size) {
	size_t           actual_size = 0u;
	syscall_status_t status;

	if (registry_cap_id == CAP_ID_INVALID)
		return local_error(REGISTRY_STATUS_TRANSPORT_ERROR, SYSCALL_STATUS_UNAVAILABLE);
	status = cap_call(registry_cap_id, request, request_size, response, response_size, &actual_size);
	if (status != SYSCALL_STATUS_OK) return local_error(REGISTRY_STATUS_TRANSPORT_ERROR, status);
	if (actual_size != response_size) return local_error(REGISTRY_STATUS_TRANSPORT_ERROR, SYSCALL_STATUS_FAILED);
	return local_error(REGISTRY_STATUS_OK, SYSCALL_STATUS_OK);
}

static struct registry_call_result registry_get_pid(process_id_t* out_pid) {
	const struct registry_get_info_request request = {.header = {.op = REGISTRY_OP_GET_INFO}};
	struct registry_get_info_response      response;
	struct registry_call_result            result;

	if (out_pid == NULL) return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	if (cached_registry_cap == registry_cap_id && cached_registry_pid != PROCESS_PID_INVALID) {
		*out_pid = cached_registry_pid;
		return local_error(REGISTRY_STATUS_OK, SYSCALL_STATUS_OK);
	}
	result = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status != REGISTRY_STATUS_OK) return result;
	if (response.status != REGISTRY_STATUS_OK || response.registry_pid == PROCESS_PID_INVALID)
		return local_error(response.status == REGISTRY_STATUS_OK ? REGISTRY_STATUS_TRANSPORT_ERROR : response.status,
		                   SYSCALL_STATUS_OK);
	cached_registry_cap = registry_cap_id;
	cached_registry_pid = response.registry_pid;
	*out_pid            = response.registry_pid;
	return local_error(REGISTRY_STATUS_OK, SYSCALL_STATUS_OK);
}

struct registry_call_result registry_advertise_with_rights(const struct registry_service_selector* selector,
                                                           uint32_t minor, cap_id_t service_capability,
                                                           cap_rights_t registry_rights, cap_rights_t client_rights) {
	struct registry_advertise_request request = {.header = {.op = REGISTRY_OP_ADVERTISE}};
	struct registry_response          response;
	struct registry_call_result       result;
	process_id_t                      registry_pid;
	cap_id_t                          delegated = CAP_ID_INVALID;
	syscall_status_t                  status;

	if (selector == NULL || service_capability == CAP_ID_INVALID || client_rights == 0u ||
	    (registry_rights & (client_rights | CAP_DELEGATE)) != (client_rights | CAP_DELEGATE))
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = registry_get_pid(&registry_pid);
	if (result.status != REGISTRY_STATUS_OK) return result;
	status = cap_delegate(service_capability, registry_pid, registry_rights, &delegated);
	if (status != SYSCALL_STATUS_OK) return local_error(REGISTRY_STATUS_RESOURCE_FAILURE, status);
	request.selector      = *selector;
	request.minor         = minor;
	request.capability    = delegated;
	request.client_rights = client_rights;
	result                = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == REGISTRY_STATUS_OK) result.status = response.status;
	if (result.status != REGISTRY_STATUS_OK) (void)cap_revoke(delegated, 0u);
	return result;
}

struct registry_call_result registry_advertise(const struct registry_service_selector* selector, uint32_t minor,
                                               cap_id_t service_capability, cap_rights_t client_rights) {
	return registry_advertise_with_rights(
		selector, minor, service_capability, client_rights | CAP_DELEGATE, client_rights);
}

struct registry_call_result registry_withdraw(const struct registry_service_selector* selector) {
	struct registry_withdraw_request request = {.header = {.op = REGISTRY_OP_WITHDRAW}};
	struct registry_response         response;
	struct registry_call_result      result;

	if (selector == NULL) return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	request.selector = *selector;
	result           = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == REGISTRY_STATUS_OK) result.status = response.status;
	return result;
}

struct registry_call_result registry_acquire(const struct registry_protocol_query* query, const char* service,
                                             struct registry_service_handle* out_handle) {
	struct registry_acquire_request  request = {.header = {.op = REGISTRY_OP_ACQUIRE}};
	struct registry_acquire_response response;
	struct registry_call_result      result;

	if (query == NULL || service == NULL || out_handle == NULL)
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	if (strlcpy(request.service, service, sizeof(request.service)) >= sizeof(request.service))
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	request.query = *query;
	result        = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status != REGISTRY_STATUS_OK) return result;
	result.status = response.status;
	if (response.status == REGISTRY_STATUS_OK) *out_handle = response.handle;
	return result;
}

struct registry_call_result registry_enumerate(const struct registry_protocol_query* query, uint64_t offset,
                                               uint64_t size, struct registry_service_info* entries,
                                               uint64_t* out_returned, uint64_t* out_total) {
	struct registry_enumerate_request   request = {.header = {.op = REGISTRY_OP_ENUMERATE}};
	struct registry_enumerate_response* response;
	struct registry_call_result         result;
	size_t                              capacity;
	size_t                              actual_size = 0u;
	syscall_status_t                    status;

	if (query == NULL || out_returned == NULL || out_total == NULL || (size != 0u && entries == NULL) ||
	    size > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*entries))
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	capacity = sizeof(*response) + (size_t)size * sizeof(*entries);
	response = malloc(capacity);
	if (response == NULL) return local_error(REGISTRY_STATUS_NO_MEMORY, SYSCALL_STATUS_FAILED);
	request.query  = *query;
	request.offset = offset;
	request.size   = size;
	status         = cap_call(registry_cap_id, &request, sizeof(request), response, capacity, &actual_size);
	if (status != SYSCALL_STATUS_OK || actual_size < sizeof(*response) || response->returned > size ||
	    actual_size != sizeof(*response) + response->returned * sizeof(*entries)) {
		free(response);
		return local_error(REGISTRY_STATUS_TRANSPORT_ERROR,
		                   status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status);
	}
	result = local_error(response->status, SYSCALL_STATUS_OK);
	if (response->status == REGISTRY_STATUS_OK) {
		if (response->returned != 0u) memcpy(entries, response->entries, response->returned * sizeof(*entries));
		*out_returned = response->returned;
		*out_total    = response->total;
	}
	free(response);
	return result;
}

struct registry_call_result registry_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                            struct registry_browse_entry* entries, uint64_t* out_returned,
                                            uint64_t* out_total) {
	struct registry_browse_request   request = {.header = {.op = REGISTRY_OP_BROWSE}};
	struct registry_browse_response* response;
	struct registry_call_result      result;
	size_t                           capacity;
	size_t                           actual_size = 0u;
	syscall_status_t                 status;

	if (namespace_path == NULL || out_returned == NULL || out_total == NULL || (size != 0u && entries == NULL) ||
	    size > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*entries) ||
	    strlcpy(request.namespace_path, namespace_path, sizeof(request.namespace_path)) >=
	        sizeof(request.namespace_path))
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	capacity = sizeof(*response) + (size_t)size * sizeof(*entries);
	response = malloc(capacity);
	if (response == NULL) return local_error(REGISTRY_STATUS_NO_MEMORY, SYSCALL_STATUS_FAILED);
	request.offset = offset;
	request.size   = size;
	status         = cap_call(registry_cap_id, &request, sizeof(request), response, capacity, &actual_size);
	if (status != SYSCALL_STATUS_OK || actual_size < sizeof(*response) || response->returned > size ||
	    actual_size != sizeof(*response) + response->returned * sizeof(*entries)) {
		free(response);
		return local_error(REGISTRY_STATUS_TRANSPORT_ERROR,
		                   status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status);
	}
	result = local_error(response->status, SYSCALL_STATUS_OK);
	if (response->status == REGISTRY_STATUS_OK) {
		if (response->returned != 0u) memcpy(entries, response->entries, response->returned * sizeof(*entries));
		*out_returned = response->returned;
		*out_total    = response->total;
	}
	free(response);
	return result;
}

struct registry_call_result registry_watch(const struct registry_protocol_query* query, cap_id_t signal_capability,
                                           uint64_t* out_subscription_id, uint64_t* out_counter) {
	struct registry_watch_request  request = {.header = {.op = REGISTRY_OP_WATCH}};
	struct registry_watch_response response;
	struct registry_call_result    result;
	process_id_t                   registry_pid;
	cap_id_t                       delegated = CAP_ID_INVALID;
	syscall_status_t               status;

	if (query == NULL || signal_capability == CAP_ID_INVALID || out_subscription_id == NULL || out_counter == NULL)
		return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = registry_get_pid(&registry_pid);
	if (result.status != REGISTRY_STATUS_OK) return result;
	status = cap_delegate(signal_capability, registry_pid, CAP_SIGNAL, &delegated);
	if (status != SYSCALL_STATUS_OK) return local_error(REGISTRY_STATUS_RESOURCE_FAILURE, status);
	request.query             = *query;
	request.signal_capability = delegated;
	result                    = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == REGISTRY_STATUS_OK) result.status = response.status;
	if (result.status != REGISTRY_STATUS_OK) {
		(void)cap_revoke(delegated, 0u);
		return result;
	}
	*out_subscription_id = response.subscription_id;
	*out_counter         = response.counter;
	return result;
}

struct registry_call_result registry_unwatch(uint64_t subscription_id) {
	struct registry_unwatch_request request = {
		.header          = {.op = REGISTRY_OP_UNWATCH},
		.subscription_id = subscription_id,
	};
	struct registry_response    response;
	struct registry_call_result result;

	if (subscription_id == 0u) return local_error(REGISTRY_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == REGISTRY_STATUS_OK) result.status = response.status;
	return result;
}
