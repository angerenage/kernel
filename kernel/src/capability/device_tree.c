#include "device_tree.h"

#include <base/cap.h>
#include <base/device_tree.h>
#include <base/math.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <firmware/dt.h>
#include <hal/device_tree.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEVICE_TREE_PROVIDER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_DELEGATE))

struct device_tree_public_node {
	struct dt_node        internal;
	device_tree_node_id_t parent;
	device_tree_node_id_t first_child;
	device_tree_node_id_t next_sibling;
	uint64_t              name_size;
	uint64_t              property_count;
};

static cap_object_id_t                       device_tree_provider_object_id = CAP_OBJECT_ID_INVALID;
static struct hal_device_tree_consumed_node* device_tree_consumed;
static size_t                                device_tree_consumed_count;
static struct device_tree_public_node*       device_tree_nodes;
static size_t                                device_tree_node_count;

static struct dt_node device_tree_walk_after(struct dt_node node) {
	while (dt_node_valid(node)) {
		struct dt_node next = dt_node_next(node);

		if (dt_node_valid(next)) return next;
		node = dt_node_parent(node);
	}
	return DT_NODE_INVALID;
}

static struct dt_node device_tree_walk_next(struct dt_node node, bool prune) {
	struct dt_node child;

	if (!dt_node_valid(node)) return DT_NODE_INVALID;
	if (!prune) {
		child = dt_node_child(node);
		if (dt_node_valid(child)) return child;
	}
	return device_tree_walk_after(node);
}

static struct hal_device_tree_consumed_node* device_tree_consumed_find(struct dt_node node) {
	for (size_t index = 0u; index < device_tree_consumed_count; index++)
		if (device_tree_consumed[index].node.id == node.id) return &device_tree_consumed[index];
	return NULL;
}

static bool device_tree_consumed_merge(struct hal_device_tree_consumed_node record) {
	struct hal_device_tree_consumed_node* existing;

	if (!dt_node_valid(record.node) || record.node.id == dt_root().id ||
	    (unsigned int)record.kind > HAL_DEVICE_TREE_REFERENCE_DMA_CONTROLLER ||
	    (record.kind == HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED && record.value != 0u) ||
	    (record.kind != HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED && record.value == 0u))
		return false;
	existing = device_tree_consumed_find(record.node);
	if (existing == NULL) {
		device_tree_consumed[device_tree_consumed_count++] = record;
		return true;
	}
	if (existing->kind == HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED) {
		if (record.kind != HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED) *existing = record;
		return true;
	}
	if (record.kind == HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED) return true;
	return existing->kind == record.kind && existing->value == record.value;
}

static bool device_tree_collect_consumed(void) {
	size_t                                upper_bound = hal_device_tree_consumed_nodes(NULL, 0u);
	size_t                                allocation_size;
	struct hal_device_tree_consumed_node* temporary;
	size_t                                written;

	if (upper_bound == 0u) return true;
	if (mul_overflow_size(upper_bound, sizeof(*temporary), &allocation_size)) return false;
	temporary = malloc(allocation_size);
	if (temporary == NULL) return false;
	device_tree_consumed = malloc(allocation_size);
	if (device_tree_consumed == NULL) {
		free(temporary);
		return false;
	}
	written = hal_device_tree_consumed_nodes(temporary, upper_bound);
	if (written > upper_bound) goto fail;
	for (size_t index = 0u; index < written; index++)
		if (!device_tree_consumed_merge(temporary[index])) goto fail;
	free(temporary);
	return true;

fail:
	free(temporary);
	free(device_tree_consumed);
	device_tree_consumed       = NULL;
	device_tree_consumed_count = 0u;
	return false;
}

static device_tree_node_id_t device_tree_public_id(struct dt_node node) {
	for (size_t index = 0u; index < device_tree_node_count; index++)
		if (device_tree_nodes[index].internal.id == node.id) return (device_tree_node_id_t)index + 1u;
	return DEVICE_TREE_NODE_INVALID;
}

static struct device_tree_public_node* device_tree_public_node(device_tree_node_id_t id) {
	if (id == DEVICE_TREE_NODE_INVALID || id == 0u || id > device_tree_node_count) return NULL;
	return &device_tree_nodes[(size_t)id - 1u];
}

static struct dt_node device_tree_first_visible_child(struct dt_node node) {
	struct dt_node child = dt_node_child(node);

	while (dt_node_valid(child) && device_tree_consumed_find(child) != NULL) child = dt_node_next(child);
	return child;
}

static struct dt_node device_tree_next_visible_sibling(struct dt_node node) {
	struct dt_node sibling = dt_node_next(node);

	while (dt_node_valid(sibling) && device_tree_consumed_find(sibling) != NULL) sibling = dt_node_next(sibling);
	return sibling;
}

static bool device_tree_build_nodes(void) {
	struct dt_node node  = dt_root();
	size_t         count = 0u;
	size_t         allocation_size;

	while (dt_node_valid(node)) {
		bool consumed = device_tree_consumed_find(node) != NULL;

		if (!consumed) {
			if (count == SIZE_MAX) return false;
			count++;
		}
		node = device_tree_walk_next(node, consumed);
	}
	if (count == 0u || mul_overflow_size(count, sizeof(*device_tree_nodes), &allocation_size)) return false;
	device_tree_nodes = calloc(1u, allocation_size);
	if (device_tree_nodes == NULL) return false;
	device_tree_node_count = count;
	node                   = dt_root();
	count                  = 0u;
	while (dt_node_valid(node)) {
		bool consumed = device_tree_consumed_find(node) != NULL;

		if (!consumed) device_tree_nodes[count++].internal = node;
		node = device_tree_walk_next(node, consumed);
	}
	for (size_t index = 0u; index < device_tree_node_count; index++) {
		struct device_tree_public_node* entry = &device_tree_nodes[index];
		struct dt_node                  parent;
		struct dt_node                  child;
		struct dt_node                  sibling;
		const char*                     name = dt_node_name(entry->internal);
		const char*                     property_name;
		struct dt_property              property;
		size_t                          properties = 0u;

		if (name == NULL) return false;
		parent              = dt_node_parent(entry->internal);
		child               = device_tree_first_visible_child(entry->internal);
		sibling             = device_tree_next_visible_sibling(entry->internal);
		entry->parent       = dt_node_valid(parent) ? device_tree_public_id(parent) : DEVICE_TREE_NODE_INVALID;
		entry->first_child  = dt_node_valid(child) ? device_tree_public_id(child) : DEVICE_TREE_NODE_INVALID;
		entry->next_sibling = dt_node_valid(sibling) ? device_tree_public_id(sibling) : DEVICE_TREE_NODE_INVALID;
		entry->name_size    = strlen(name);
		while (dt_node_property_at(entry->internal, properties, &property_name, &property)) {
			if (properties == SIZE_MAX) return false;
			properties++;
		}
		entry->property_count = properties;
	}
	return true;
}

static syscall_result_t device_tree_root_handler(const struct cap_request* req) {
	const struct device_tree_root_request request_expected = {.header = {.op = DEVICE_TREE_OP_ROOT}};
	struct device_tree_root_request       request;
	struct device_tree_root_response      response = {.root = 1u};

	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (memcmp(&request, &request_expected, sizeof(request)) != 0)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t device_tree_node_info_handler(const struct cap_request* req) {
	struct device_tree_node_info_request  request;
	struct device_tree_node_info_response response = {0};
	struct device_tree_public_node*       node;

	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	node = device_tree_public_node(request.node);
	if (request.header.op != DEVICE_TREE_OP_NODE_INFO || request.reserved != 0u || node == NULL)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response = (struct device_tree_node_info_response){
		.parent         = node->parent,
		.first_child    = node->first_child,
		.next_sibling   = node->next_sibling,
		.name_size      = node->name_size,
		.property_count = node->property_count,
	};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t device_tree_property_info_handler(const struct cap_request* req) {
	struct device_tree_property_info_request  request;
	struct device_tree_property_info_response response = {0};
	struct device_tree_public_node*           node;
	const char*                               name;
	struct dt_property                        property;

	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	node = device_tree_public_node(request.node);
	if (request.header.op != DEVICE_TREE_OP_PROPERTY_INFO || request.reserved != 0u || node == NULL ||
	    request.property_index > SIZE_MAX || request.property_index >= node->property_count ||
	    !dt_node_property_at(node->internal, (size_t)request.property_index, &name, &property))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response.name_size  = strlen(name);
	response.value_size = property.size;
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t device_tree_read_handler(const struct cap_request* req) {
	struct device_tree_read_request request;
	struct device_tree_public_node* node;
	const void*                     data;
	size_t                          data_size;
	const char*                     property_name;
	struct dt_property              property;
	size_t                          offset;
	size_t                          size;

	if (req->request == NULL || req->request_size != sizeof(request))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	node = device_tree_public_node(request.node);
	if (request.header.op != DEVICE_TREE_OP_READ || (unsigned int)request.kind > DEVICE_TREE_READ_PROPERTY_VALUE ||
	    node == NULL || request.offset > SIZE_MAX || request.size > SIZE_MAX || request.size > CAP_MAX_RESPONSE_SIZE)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (request.kind == DEVICE_TREE_READ_NODE_NAME) {
		if (request.property_index != 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		data      = dt_node_name(node->internal);
		data_size = (size_t)node->name_size;
	}
	else {
		if (request.property_index > SIZE_MAX || request.property_index >= node->property_count ||
		    !dt_node_property_at(node->internal, (size_t)request.property_index, &property_name, &property))
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		if (request.kind == DEVICE_TREE_READ_PROPERTY_NAME) {
			data      = property_name;
			data_size = strlen(property_name);
		}
		else {
			data      = property.data;
			data_size = property.size;
		}
	}
	offset = (size_t)request.offset;
	size   = (size_t)request.size;
	if (offset > data_size || size > data_size - offset) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (size == 0u) return syscall_result_ok(0u);
	if (!cap_kernel_response_fits(req, size)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return cap_kernel_write_response(req, (const uint8_t*)data + offset, size);
}

static syscall_result_t device_tree_resolve_phandle_handler(const struct cap_request* req) {
	struct device_tree_resolve_phandle_request  request;
	struct device_tree_resolve_phandle_response response = {0};
	struct hal_device_tree_consumed_node*       consumed;
	struct dt_node                              node;
	device_tree_node_id_t                       public_id;

	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != DEVICE_TREE_OP_RESOLVE_PHANDLE || request.phandle == 0u)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	node = dt_node_by_phandle(request.phandle);
	if (!dt_node_valid(node)) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	public_id = device_tree_public_id(node);
	if (public_id != DEVICE_TREE_NODE_INVALID) {
		response.kind  = DEVICE_TREE_REFERENCE_NODE;
		response.value = public_id;
		return cap_kernel_write_response(req, &response, sizeof(response));
	}
	consumed = device_tree_consumed_find(node);
	if (consumed == NULL || consumed->kind == HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED) {
		response.kind = DEVICE_TREE_REFERENCE_UNSUPPORTED;
	}
	else if (consumed->kind == HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER) {
		response.kind  = DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER;
		response.value = consumed->value;
	}
	else {
		response.kind  = DEVICE_TREE_REFERENCE_DMA_CONTROLLER;
		response.value = consumed->value;
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t device_tree_provider_handler(const struct cap_request* req) {
	struct device_tree_request_header header;

	if (req == NULL || (req->rights & CAP_READ) == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(
			req != NULL && (req->rights & CAP_READ) == 0u ? SYSCALL_STATUS_DENIED : SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	switch (header.op) {
	case DEVICE_TREE_OP_ROOT:
		return device_tree_root_handler(req);
	case DEVICE_TREE_OP_NODE_INFO:
		return device_tree_node_info_handler(req);
	case DEVICE_TREE_OP_PROPERTY_INFO:
		return device_tree_property_info_handler(req);
	case DEVICE_TREE_OP_READ:
		return device_tree_read_handler(req);
	case DEVICE_TREE_OP_RESOLVE_PHANDLE:
		return device_tree_resolve_phandle_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_device_tree_init(void) {
	if (!dt_node_valid(dt_root()) || device_tree_provider_object_id != CAP_OBJECT_ID_INVALID) return true;
	if (!device_tree_collect_consumed() || !device_tree_build_nodes()) goto fail;
	device_tree_provider_object_id = cap_object_create_kernel(0u, device_tree_provider_handler, NULL);
	if (device_tree_provider_object_id == CAP_OBJECT_ID_INVALID) goto fail;
	return true;

fail:
	free(device_tree_nodes);
	free(device_tree_consumed);
	device_tree_nodes          = NULL;
	device_tree_node_count     = 0u;
	device_tree_consumed       = NULL;
	device_tree_consumed_count = 0u;
	return false;
}

bool kernel_capability_device_tree_available(void) {
	return dt_node_valid(dt_root()) && device_tree_provider_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_device_tree_grant(process_id_t recipient) {
	if (!kernel_capability_device_tree_available() || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(device_tree_provider_object_id, recipient, DEVICE_TREE_PROVIDER_RIGHTS, NULL);
}
