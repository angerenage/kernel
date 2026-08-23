#include <libc/stdlib.h>
#include <libc/string.h>
#include <protocol/loader.h>
#include <runtime/init.h>
#include <runtime/program.h>
#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>

#define LOADER_PROTOCOL_VERSION_MAJOR 1u
#define LOADER_PROTOCOL_VERSION_MINOR 0u

struct cached_loader {
	char         service[INIT_NAME_MAX + 1u];
	process_id_t owner;
	cap_id_t     capability;
};

static struct cached_loader loader_cache = {
	.owner      = PROCESS_PID_INVALID,
	.capability = CAP_ID_INVALID,
};

static syscall_status_t registry_result_status(struct init_call_result result) {
	if (result.transport_status != SYSCALL_STATUS_OK) return result.transport_status;
	switch (result.status) {
	case INIT_REGISTRY_OK:
		return SYSCALL_STATUS_OK;
	case INIT_REGISTRY_INVALID_ARGUMENT:
		return SYSCALL_STATUS_BAD_ARGUMENT;
	case INIT_REGISTRY_NOT_FOUND:
		return SYSCALL_STATUS_UNAVAILABLE;
	case INIT_REGISTRY_DENIED:
		return SYSCALL_STATUS_DENIED;
	default:
		return SYSCALL_STATUS_FAILED;
	}
}

static bool cached_loader_matches(const char* service) {
	bool             valid = false;
	syscall_status_t status;

	if (loader_cache.capability == CAP_ID_INVALID || loader_cache.owner == PROCESS_PID_INVALID ||
	    strcmp(loader_cache.service, service) != 0)
		return false;
	status = cap_valid(loader_cache.capability, &valid);
	return status == SYSCALL_STATUS_OK && valid;
}

static syscall_status_t resolve_loader(const char* service, struct cached_loader* out_loader) {
	static const struct init_protocol_query query = {
		.namespace_path = PROGRAM_LOADER_NAMESPACE,
		.protocol       = LOADER_PROTOCOL_NAME,
		.major          = LOADER_PROTOCOL_VERSION_MAJOR,
		.minor          = LOADER_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle handle;
	struct init_call_result    result;
	struct cached_loader       replacement;
	syscall_status_t           cleanup_status;

	if (service == NULL || out_loader == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (cached_loader_matches(service)) {
		*out_loader = loader_cache;
		return SYSCALL_STATUS_OK;
	}

	result = init_acquire(&query, service, &handle);
	if (result.status != INIT_REGISTRY_OK || result.transport_status != SYSCALL_STATUS_OK)
		return registry_result_status(result);
	if (handle.capability == CAP_ID_INVALID || handle.owner == PROCESS_PID_INVALID) return SYSCALL_STATUS_FAILED;

	replacement = (struct cached_loader){
		.owner      = handle.owner,
		.capability = handle.capability,
	};
	(void)strlcpy(replacement.service, handle.info.selector.service, sizeof(replacement.service));

	if (loader_cache.capability != CAP_ID_INVALID && loader_cache.capability != replacement.capability) {
		cleanup_status = cap_drop(loader_cache.capability);
		if (cleanup_status != SYSCALL_STATUS_OK) {
			(void)cap_drop(replacement.capability);
			return cleanup_status;
		}
	}

	loader_cache = replacement;
	*out_loader  = loader_cache;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t loader_load(cap_id_t loader_cap, cap_id_t blob_cap, const char* name, size_t name_size,
                                    struct program_load_result* out_result) {
	struct loader_v1_load_request* request;
	struct loader_v1_load_response response;
	size_t                         request_size;
	size_t                         response_size = 0u;
	syscall_status_t               status;

	if (name_size > UINT32_MAX || name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + name_size;
	request      = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;

	*request = (struct loader_v1_load_request){
		.header    = {.op = LOADER_V1_OP_LOAD},
		.blob_cap  = blob_cap,
		.name_size = (uint32_t)name_size,
	};
	memcpy(request + 1, name, name_size);
	status = cap_call(loader_cap, request, request_size, &response, sizeof(response), &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.load_cap == CAP_ID_INVALID ||
	    response.process_id == PROCESS_PID_INVALID)
		return SYSCALL_STATUS_FAILED;
	*out_result = (struct program_load_result){
		.load_cap   = response.load_cap,
		.process_id = response.process_id,
	};
	return SYSCALL_STATUS_OK;
}

syscall_status_t program_load(const char* service, cap_id_t blob_cap, const char* name, size_t name_size,
                              struct program_load_result* out_result) {
	struct cached_loader loader;
	cap_id_t             delegated_blob = CAP_ID_INVALID;
	syscall_status_t     cleanup_status;
	syscall_status_t     status;

	if (service == NULL || blob_cap == CAP_ID_INVALID || name == NULL || name_size == 0u || out_result == NULL ||
	    memchr(name, '\0', name_size) != NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_result = (struct program_load_result){
		.load_cap   = CAP_ID_INVALID,
		.process_id = PROCESS_PID_INVALID,
	};

	status = resolve_loader(service, &loader);
	if (status != SYSCALL_STATUS_OK) return status;
	status = cap_delegate(blob_cap, loader.owner, LOADER_V1_BLOB_CAP_RIGHTS, &delegated_blob);
	if (status != SYSCALL_STATUS_OK) return status;
	status         = loader_load(loader.capability, delegated_blob, name, name_size, out_result);
	cleanup_status = cap_revoke(delegated_blob, 0u);
	if (cleanup_status != SYSCALL_STATUS_OK) {
		if (status == SYSCALL_STATUS_OK && out_result->load_cap != CAP_ID_INVALID &&
		    program_cancel(out_result->load_cap) == SYSCALL_STATUS_OK) {
			*out_result = (struct program_load_result){
				.load_cap   = CAP_ID_INVALID,
				.process_id = PROCESS_PID_INVALID,
			};
		}
		return cleanup_status;
	}
	return status;
}

static syscall_status_t argv_wire_size(size_t argc, const char* const argv[], size_t* out_size) {
	size_t total = 0u;

	if (out_size == NULL || argc > UINT32_MAX || (argc != 0u && argv == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;
	for (size_t i = 0u; i < argc; i++) {
		size_t length;
		if (argv[i] == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
		length = strlen(argv[i]) + 1u;
		if (length > UINT32_MAX - total) return SYSCALL_STATUS_BAD_ARGUMENT;
		total += length;
	}
	*out_size = total;
	return SYSCALL_STATUS_OK;
}

syscall_status_t program_run(cap_id_t load_cap, size_t argc, const char* const argv[],
                             struct program_run_result* out_result) {
	struct loader_v1_run_request* request;
	struct loader_v1_run_response response;
	char*                         cursor;
	size_t                        argv_size;
	size_t                        request_size;
	size_t                        response_size = 0u;
	syscall_status_t              status;

	if (load_cap == CAP_ID_INVALID || out_result == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_result = (struct program_run_result){
		.process_cap = CAP_ID_INVALID,
		.thread_cap  = CAP_ID_INVALID,
	};
	status = argv_wire_size(argc, argv, &argv_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (argv_size > CAP_MAX_REQUEST_SIZE - sizeof(*request)) return SYSCALL_STATUS_BAD_ARGUMENT;

	request_size = sizeof(*request) + argv_size;
	request      = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	*request = (struct loader_v1_run_request){
		.header    = {.op = LOADER_V1_OP_RUN},
		.argc      = (uint32_t)argc,
		.argv_size = (uint32_t)argv_size,
	};
	cursor = (char*)(request + 1);
	for (size_t i = 0u; i < argc; i++) {
		size_t length = strlen(argv[i]) + 1u;
		memcpy(cursor, argv[i], length);
		cursor += length;
	}

	status = cap_call(load_cap, request, request_size, &response, sizeof(response), &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.process_cap == CAP_ID_INVALID ||
	    response.thread_cap == CAP_ID_INVALID)
		return SYSCALL_STATUS_FAILED;
	*out_result = (struct program_run_result){
		.process_cap = response.process_cap,
		.thread_cap  = response.thread_cap,
	};
	return SYSCALL_STATUS_OK;
}

syscall_status_t program_cancel(cap_id_t load_cap) {
	const struct loader_v1_cancel_request request = {
		.header = {.op = LOADER_V1_OP_CANCEL},
	};

	if (load_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return cap_call(load_cap, &request, sizeof(request), NULL, 0u, NULL);
}
