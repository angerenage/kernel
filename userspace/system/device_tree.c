#include <base/cap.h>
#include <base/device_tree.h>
#include <base/syscall.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/device_tree.h>

static syscall_status_t device_tree_fixed_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                               size_t response_size, uint64_t operation) {
	(void)operation;
	if (cap == CAP_ID_INVALID || request == NULL || (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(cap, request, request_size, response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t device_tree_root(cap_id_t provider_cap, device_tree_node_id_t* out_root) {
	const struct device_tree_root_request request  = {.header = {.op = DEVICE_TREE_OP_ROOT}};
	struct device_tree_root_response      response = {.root = DEVICE_TREE_NODE_INVALID};

	if (out_root == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_root               = DEVICE_TREE_NODE_INVALID;
	syscall_status_t status = device_tree_fixed_call(
		provider_cap, &request, sizeof(request), &response, sizeof(response), DEVICE_TREE_OP_ROOT);
	if (status == SYSCALL_STATUS_OK) {
		if (response.root == DEVICE_TREE_NODE_INVALID || response.root == 0u) return SYSCALL_STATUS_FAILED;
		*out_root = response.root;
	}
	return status;
}

syscall_status_t device_tree_node_info(cap_id_t provider_cap, device_tree_node_id_t node,
                                       struct device_tree_node_info_response* out_info) {
	const struct device_tree_node_info_request request = {
		.header = {.op = DEVICE_TREE_OP_NODE_INFO}, .reserved = 0u, .node = node};

	if (out_info == NULL || node == DEVICE_TREE_NODE_INVALID || node == 0u) return SYSCALL_STATUS_BAD_ARGUMENT;
	return device_tree_fixed_call(
		provider_cap, &request, sizeof(request), out_info, sizeof(*out_info), DEVICE_TREE_OP_NODE_INFO);
}

syscall_status_t device_tree_property_info(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           struct device_tree_property_info_response* out_info) {
	const struct device_tree_property_info_request request = {
		.header         = {.op = DEVICE_TREE_OP_PROPERTY_INFO},
		.reserved       = 0u,
		.node           = node,
		.property_index = property_index,
	};

	if (out_info == NULL || node == DEVICE_TREE_NODE_INVALID || node == 0u) return SYSCALL_STATUS_BAD_ARGUMENT;
	return device_tree_fixed_call(
		provider_cap, &request, sizeof(request), out_info, sizeof(*out_info), DEVICE_TREE_OP_PROPERTY_INFO);
}

static syscall_status_t device_tree_read(cap_id_t provider_cap, enum device_tree_read_kind kind,
                                         device_tree_node_id_t node, uint64_t property_index, uint64_t offset,
                                         void* buffer, size_t size) {
	const struct device_tree_read_request request = {
		.header         = {.op = DEVICE_TREE_OP_READ},
		.kind           = kind,
		.node           = node,
		.property_index = property_index,
		.offset         = offset,
		.size           = size,
	};

	if (provider_cap == CAP_ID_INVALID || node == DEVICE_TREE_NODE_INVALID || node == 0u ||
	    (size != 0u && buffer == NULL) || size > CAP_MAX_RESPONSE_SIZE)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(provider_cap, &request, sizeof(request), buffer, size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(DEVICE_TREE_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t device_tree_node_name_read(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t offset,
                                            void* buffer, size_t size) {
	return device_tree_read(provider_cap, DEVICE_TREE_READ_NODE_NAME, node, 0u, offset, buffer, size);
}

syscall_status_t device_tree_property_name_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                                uint64_t property_index, uint64_t offset, void* buffer, size_t size) {
	return device_tree_read(provider_cap, DEVICE_TREE_READ_PROPERTY_NAME, node, property_index, offset, buffer, size);
}

syscall_status_t device_tree_property_read(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           uint64_t offset, void* buffer, size_t size) {
	return device_tree_read(provider_cap, DEVICE_TREE_READ_PROPERTY_VALUE, node, property_index, offset, buffer, size);
}

syscall_status_t device_tree_resolve_phandle(cap_id_t provider_cap, uint32_t phandle,
                                             struct device_tree_resolve_phandle_response* out_reference) {
	const struct device_tree_resolve_phandle_request request = {.header  = {.op = DEVICE_TREE_OP_RESOLVE_PHANDLE},
	                                                            .phandle = phandle};

	if (out_reference == NULL || phandle == 0u) return SYSCALL_STATUS_BAD_ARGUMENT;
	return device_tree_fixed_call(
		provider_cap, &request, sizeof(request), out_reference, sizeof(*out_reference), DEVICE_TREE_OP_RESOLVE_PHANDLE);
}
