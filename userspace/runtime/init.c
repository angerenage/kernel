#include <base/cap.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <system/capability.h>

cap_id_t init_cap_id = CAP_ID_INVALID;

static cap_id_t     cached_init_cap = CAP_ID_INVALID;
static process_id_t cached_init_pid = PROCESS_PID_INVALID;

static struct init_call_result local_error(enum init_registry_status status, syscall_status_t transport) {
	return (struct init_call_result){.status = status, .transport_status = transport};
}

static struct init_call_result call_fixed(const void* request, size_t request_size, void* response,
                                          size_t response_size) {
	size_t           actual_size = 0u;
	syscall_status_t status;

	if (init_cap_id == CAP_ID_INVALID) return local_error(INIT_REGISTRY_TRANSPORT_ERROR, SYSCALL_STATUS_UNAVAILABLE);
	status = cap_call(init_cap_id, request, request_size, response, response_size, &actual_size);
	if (status != SYSCALL_STATUS_OK) return local_error(INIT_REGISTRY_TRANSPORT_ERROR, status);
	if (actual_size != response_size) return local_error(INIT_REGISTRY_TRANSPORT_ERROR, SYSCALL_STATUS_FAILED);
	return local_error(INIT_REGISTRY_OK, SYSCALL_STATUS_OK);
}

static struct init_call_result init_get_pid(process_id_t* out_pid) {
	const struct init_get_info_request request = {.header = {.op = INIT_OP_GET_INFO}};
	struct init_get_info_response      response;
	struct init_call_result            result;

	if (out_pid == NULL) return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	if (cached_init_cap == init_cap_id && cached_init_pid != PROCESS_PID_INVALID) {
		*out_pid = cached_init_pid;
		return local_error(INIT_REGISTRY_OK, SYSCALL_STATUS_OK);
	}
	result = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status != INIT_REGISTRY_OK) return result;
	if (response.status != INIT_REGISTRY_OK || response.init_pid == PROCESS_PID_INVALID)
		return local_error(response.status == INIT_REGISTRY_OK ? INIT_REGISTRY_TRANSPORT_ERROR : response.status,
		                   SYSCALL_STATUS_OK);
	cached_init_cap = init_cap_id;
	cached_init_pid = response.init_pid;
	*out_pid        = response.init_pid;
	return local_error(INIT_REGISTRY_OK, SYSCALL_STATUS_OK);
}

struct init_call_result init_advertise(const struct init_service_selector* selector, uint32_t minor,
                                       cap_id_t service_capability, cap_rights_t client_rights) {
	struct init_advertise_request request = {.header = {.op = INIT_OP_ADVERTISE}};
	struct init_registry_response response;
	struct init_call_result       result;
	process_id_t                  init_pid;
	cap_id_t                      delegated = CAP_ID_INVALID;
	syscall_status_t              status;

	if (selector == NULL || service_capability == CAP_ID_INVALID || client_rights == 0u)
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = init_get_pid(&init_pid);
	if (result.status != INIT_REGISTRY_OK) return result;
	status = cap_delegate(service_capability, init_pid, client_rights | CAP_DELEGATE, &delegated);
	if (status != SYSCALL_STATUS_OK) return local_error(INIT_REGISTRY_RESOURCE_FAILURE, status);
	request.selector      = *selector;
	request.minor         = minor;
	request.capability    = delegated;
	request.client_rights = client_rights;
	result                = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == INIT_REGISTRY_OK) result.status = response.status;
	if (result.status != INIT_REGISTRY_OK) (void)cap_revoke(delegated, 0u);
	return result;
}

struct init_call_result init_withdraw(const struct init_service_selector* selector) {
	struct init_withdraw_request  request = {.header = {.op = INIT_OP_WITHDRAW}};
	struct init_registry_response response;
	struct init_call_result       result;

	if (selector == NULL) return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	request.selector = *selector;
	result           = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == INIT_REGISTRY_OK) result.status = response.status;
	return result;
}

struct init_call_result init_lookup(const struct init_protocol_query* query, const char* service,
                                    struct init_service_entry* out_entry) {
	struct init_lookup_request  request = {.header = {.op = INIT_OP_LOOKUP}};
	struct init_lookup_response response;
	struct init_call_result     result;

	if (query == NULL || service == NULL || out_entry == NULL)
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	if (strlcpy(request.service, service, sizeof(request.service)) >= sizeof(request.service))
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	request.query = *query;
	result        = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status != INIT_REGISTRY_OK) return result;
	result.status = response.status;
	if (response.status == INIT_REGISTRY_OK) *out_entry = response.entry;
	return result;
}

struct init_call_result init_enumerate(const struct init_protocol_query* query, uint64_t offset, uint64_t size,
                                       struct init_service_entry* entries, uint64_t* out_returned,
                                       uint64_t* out_total) {
	struct init_enumerate_request   request = {.header = {.op = INIT_OP_ENUMERATE}};
	struct init_enumerate_response* response;
	struct init_call_result         result;
	size_t                          capacity;
	size_t                          actual_size = 0u;
	syscall_status_t                status;

	if (query == NULL || out_returned == NULL || out_total == NULL || (size != 0u && entries == NULL) ||
	    size > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*entries))
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	capacity = sizeof(*response) + (size_t)size * sizeof(*entries);
	response = malloc(capacity);
	if (response == NULL) return local_error(INIT_REGISTRY_NO_MEMORY, SYSCALL_STATUS_FAILED);
	request.query  = *query;
	request.offset = offset;
	request.size   = size;
	status         = cap_call(init_cap_id, &request, sizeof(request), response, capacity, &actual_size);
	if (status != SYSCALL_STATUS_OK || actual_size < sizeof(*response) || response->returned > size ||
	    actual_size != sizeof(*response) + response->returned * sizeof(*entries)) {
		free(response);
		return local_error(INIT_REGISTRY_TRANSPORT_ERROR, status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status);
	}
	result = local_error(response->status, SYSCALL_STATUS_OK);
	if (response->status == INIT_REGISTRY_OK) {
		if (response->returned != 0u) memcpy(entries, response->entries, response->returned * sizeof(*entries));
		*out_returned = response->returned;
		*out_total    = response->total;
	}
	free(response);
	return result;
}

struct init_call_result init_browse(const char* namespace_path, uint64_t offset, uint64_t size,
                                    struct init_browse_entry* entries, uint64_t* out_returned, uint64_t* out_total) {
	struct init_browse_request   request = {.header = {.op = INIT_OP_BROWSE}};
	struct init_browse_response* response;
	struct init_call_result      result;
	size_t                       capacity;
	size_t                       actual_size = 0u;
	syscall_status_t             status;

	if (namespace_path == NULL || out_returned == NULL || out_total == NULL || (size != 0u && entries == NULL) ||
	    size > (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(*entries) ||
	    strlcpy(request.namespace_path, namespace_path, sizeof(request.namespace_path)) >=
	        sizeof(request.namespace_path))
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	capacity = sizeof(*response) + (size_t)size * sizeof(*entries);
	response = malloc(capacity);
	if (response == NULL) return local_error(INIT_REGISTRY_NO_MEMORY, SYSCALL_STATUS_FAILED);
	request.offset = offset;
	request.size   = size;
	status         = cap_call(init_cap_id, &request, sizeof(request), response, capacity, &actual_size);
	if (status != SYSCALL_STATUS_OK || actual_size < sizeof(*response) || response->returned > size ||
	    actual_size != sizeof(*response) + response->returned * sizeof(*entries)) {
		free(response);
		return local_error(INIT_REGISTRY_TRANSPORT_ERROR, status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status);
	}
	result = local_error(response->status, SYSCALL_STATUS_OK);
	if (response->status == INIT_REGISTRY_OK) {
		if (response->returned != 0u) memcpy(entries, response->entries, response->returned * sizeof(*entries));
		*out_returned = response->returned;
		*out_total    = response->total;
	}
	free(response);
	return result;
}

struct init_call_result init_watch(const struct init_protocol_query* query, cap_id_t signal_capability,
                                   uint64_t* out_subscription_id, uint64_t* out_counter) {
	struct init_watch_request  request = {.header = {.op = INIT_OP_WATCH}};
	struct init_watch_response response;
	struct init_call_result    result;
	process_id_t               init_pid;
	cap_id_t                   delegated = CAP_ID_INVALID;
	syscall_status_t           status;

	if (query == NULL || signal_capability == CAP_ID_INVALID || out_subscription_id == NULL || out_counter == NULL)
		return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = init_get_pid(&init_pid);
	if (result.status != INIT_REGISTRY_OK) return result;
	status = cap_delegate(signal_capability, init_pid, CAP_SIGNAL, &delegated);
	if (status != SYSCALL_STATUS_OK) return local_error(INIT_REGISTRY_RESOURCE_FAILURE, status);
	request.query             = *query;
	request.signal_capability = delegated;
	result                    = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == INIT_REGISTRY_OK) result.status = response.status;
	if (result.status != INIT_REGISTRY_OK) {
		(void)cap_revoke(delegated, 0u);
		return result;
	}
	*out_subscription_id = response.subscription_id;
	*out_counter         = response.counter;
	return result;
}

struct init_call_result init_unwatch(uint64_t subscription_id) {
	struct init_unwatch_request request = {
		.header          = {.op = INIT_OP_UNWATCH},
		.subscription_id = subscription_id,
	};
	struct init_registry_response response;
	struct init_call_result       result;

	if (subscription_id == 0u) return local_error(INIT_REGISTRY_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
	result = call_fixed(&request, sizeof(request), &response, sizeof(response));
	if (result.status == INIT_REGISTRY_OK) result.status = response.status;
	return result;
}
