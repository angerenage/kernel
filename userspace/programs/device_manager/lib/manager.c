#include <protocol/device_manager_control.h>
#include <runtime/device_manager.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

syscall_status_t device_manager_catalog_info(cap_id_t manager_cap, struct device_manager_catalog_info* out_info) {
	const struct device_manager_catalog_info_request request = {
		.header = {.op = DEVICE_MANAGER_OP_CATALOG_INFO},
	};
	size_t           response_size = 0u;
	syscall_status_t status;

	if (manager_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_call(manager_cap, &request, sizeof(request), out_info, sizeof(*out_info), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(*out_info) || out_info->generation == 0u) return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t device_manager_update_catalog(cap_id_t manager_cap, const char* path, size_t path_size,
                                               struct device_manager_update_catalog_response* out_response) {
	struct device_manager_update_catalog_request* request;
	size_t                                        request_size;
	size_t                                        response_size = 0u;
	syscall_status_t                              status;

	if (manager_cap == CAP_ID_INVALID || path == NULL || path_size == 0u || path_size > UINT32_MAX ||
	    path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) || out_response == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + path_size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = DEVICE_MANAGER_OP_UPDATE_CATALOG;
	request->path_size = (uint32_t)path_size;
	memcpy(request->path, path, path_size);
	status = cap_call(manager_cap, request, request_size, out_response, sizeof(*out_response), &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(*out_response) || out_response->reserved != 0u ||
	    out_response->status > DEVICE_MANAGER_CATALOG_UPDATE_NO_MEMORY || out_response->catalog.generation == 0u)
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}
