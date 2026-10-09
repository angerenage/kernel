#include <protocol/device.h>
#include <runtime/device.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/memory.h>

static bool identifier_valid(const void* value, size_t size) {
	const uint8_t* bytes = value;
	if (bytes == NULL || size == 0u || size > DEVICE_IDENTIFIER_MAX || bytes[0] < 'a' || bytes[0] > 'z') return false;
	for (size_t index = 1u; index < size; index++) {
		uint8_t byte = bytes[index];
		if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '_') return false;
	}
	return true;
}

static bool utf8_valid(const void* value, size_t size) {
	const uint8_t* bytes = value;
	if (bytes == NULL && size != 0u) return false;
	for (size_t index = 0u; index < size;) {
		uint8_t  first = bytes[index++];
		uint32_t codepoint;
		size_t   trailing;
		if (first == 0u) return false;
		if (first < 0x80u) continue;
		if (first >= 0xc2u && first <= 0xdfu) {
			codepoint = first & 0x1fu;
			trailing  = 1u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			codepoint = first & 0x0fu;
			trailing  = 2u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			codepoint = first & 0x07u;
			trailing  = 3u;
		}
		else return false;
		if (trailing > size - index) return false;
		for (size_t part = 0u; part < trailing; part++) {
			uint8_t byte = bytes[index++];
			if ((byte & 0xc0u) != 0x80u) return false;
			codepoint = (codepoint << 6u) | (byte & 0x3fu);
		}
		if ((trailing == 1u && codepoint < 0x80u) || (trailing == 2u && codepoint < 0x800u) ||
		    (trailing == 3u && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
		    (codepoint >= 0xd800u && codepoint <= 0xdfffu))
			return false;
	}
	return true;
}

static bool property_shape_valid(const struct device_property_info* info) {
	if (info->reserved != 0u) return false;
	switch (info->type) {
	case DEVICE_PROPERTY_BOOLEAN:
		return info->element_count == 1u && info->value_size == 1u;
	case DEVICE_PROPERTY_UNSIGNED:
	case DEVICE_PROPERTY_SIGNED:
		return info->element_count == 1u && info->value_size == 8u;
	case DEVICE_PROPERTY_UTF8_STRING:
		return info->element_count == 1u;
	case DEVICE_PROPERTY_BYTES:
	case DEVICE_PROPERTY_BOOLEAN_ARRAY:
		return info->element_count == info->value_size;
	case DEVICE_PROPERTY_UNSIGNED_ARRAY:
	case DEVICE_PROPERTY_SIGNED_ARRAY:
		return info->element_count <= UINT64_MAX / 8u && info->value_size == info->element_count * 8u;
	case DEVICE_PROPERTY_UTF8_STRING_ARRAY:
		return info->element_count <= info->value_size / 4u;
	default:
		return false;
	}
}

static syscall_status_t call_exact(cap_id_t cap, const void* request, size_t request_size, void* response,
                                   size_t response_size) {
	size_t           actual = 0u;
	syscall_status_t status;

	if (cap == CAP_ID_INVALID || request == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_call(cap, request, request_size, response, response_size, &actual);
	if (status != SYSCALL_STATUS_OK) return status;
	return actual == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

static syscall_status_t call_empty(cap_id_t cap, const void* request, size_t request_size) {
	return call_exact(cap, request, request_size, NULL, 0u);
}

static syscall_status_t begin_builder(cap_id_t cap, uint32_t operation, struct device_builder* out_builder) {
	struct device_builder_begin_response response = {0};
	syscall_status_t                     status;

	if (out_builder == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_builder = (struct device_builder){.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	status       = call_exact(cap, &operation, sizeof(operation), &response, sizeof(response));
	if (status != SYSCALL_STATUS_OK) return status;
	if (response.builder_cap == CAP_ID_INVALID || response.manager_pid == PROCESS_PID_INVALID)
		return SYSCALL_STATUS_FAILED;
	out_builder->capability  = response.builder_cap;
	out_builder->manager_pid = response.manager_pid;
	return SYSCALL_STATUS_OK;
}

syscall_status_t device_builder_begin_root(cap_id_t root_cap, struct device_builder* out_builder) {
	return begin_builder(root_cap, DEVICE_ROOT_OP_BEGIN, out_builder);
}

syscall_status_t device_builder_begin_child(cap_id_t device_cap, struct device_builder* out_builder) {
	return begin_builder(device_cap, DEVICE_OBJECT_OP_BEGIN_CHILD, out_builder);
}

static syscall_status_t builder_string_call(const struct device_builder* builder, uint32_t operation, const char* value,
                                            size_t value_size, size_t header_size) {
	uint8_t*         request;
	syscall_status_t status;

	if (builder == NULL || builder->capability == CAP_ID_INVALID || value == NULL || value_size > UINT32_MAX ||
	    header_size > CAP_MAX_REQUEST_SIZE || value_size > CAP_MAX_REQUEST_SIZE - header_size)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request = calloc(1u, header_size + value_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	memcpy(request, &operation, sizeof(operation));
	memcpy(request + sizeof(operation), &(uint32_t){(uint32_t)value_size}, sizeof(uint32_t));
	memcpy(request + header_size, value, value_size);
	status = call_empty(builder->capability, request, header_size + value_size);
	free(request);
	return status;
}

syscall_status_t device_builder_set_name(const struct device_builder* builder, const char* name, size_t name_size) {
	if (name_size == 0u || name_size > DEVICE_NAME_MAX || !utf8_valid(name, name_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	return builder_string_call(
		builder, DEVICE_BUILDER_OP_SET_NAME, name, name_size, sizeof(struct device_builder_set_name_request));
}

syscall_status_t device_builder_add_compatible(const struct device_builder* builder, const char* id, size_t id_size) {
	if (id_size == 0u || id_size > DEVICE_COMPATIBLE_ID_MAX || !utf8_valid(id, id_size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	return builder_string_call(
		builder, DEVICE_BUILDER_OP_ADD_COMPATIBLE, id, id_size, sizeof(struct device_builder_add_compatible_request));
}

syscall_status_t device_builder_begin_property(const struct device_builder* builder, const char* name, size_t name_size,
                                               enum device_property_type type, uint64_t element_count,
                                               uint64_t value_size) {
	struct device_builder_begin_property_request* request;
	size_t                                        request_size;
	syscall_status_t                              status;

	const struct device_property_info info = {.type = type, .element_count = element_count, .value_size = value_size};
	if (builder == NULL || builder->capability == CAP_ID_INVALID || !identifier_valid(name, name_size) ||
	    !property_shape_valid(&info) || name_size > UINT32_MAX || name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + name_size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	*request = (struct device_builder_begin_property_request){
		.header        = {.op = DEVICE_BUILDER_OP_BEGIN_PROPERTY},
		.type          = type,
		.name_size     = (uint32_t)name_size,
		.element_count = element_count,
		.value_size    = value_size,
	};
	memcpy(request->name, name, name_size);
	status = call_empty(builder->capability, request, request_size);
	free(request);
	return status;
}

syscall_status_t device_builder_append_property(const struct device_builder* builder, uint64_t offset,
                                                const void* bytes, size_t size) {
	struct device_builder_append_property_request* request;
	size_t                                         request_size;
	syscall_status_t                               status;

	if (builder == NULL || builder->capability == CAP_ID_INVALID || (size != 0u && bytes == NULL) ||
	    size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	*request = (struct device_builder_append_property_request){
		.header = {.op = DEVICE_BUILDER_OP_APPEND_PROPERTY},
		.offset = offset,
	};
	if (size != 0u) memcpy(request->bytes, bytes, size);
	status = call_empty(builder->capability, request, request_size);
	free(request);
	return status;
}

static syscall_status_t builder_simple(const struct device_builder* builder, enum device_builder_op op) {
	const struct device_builder_simple_request request = {.header = {.op = op}};
	if (builder == NULL || builder->capability == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return call_empty(builder->capability, &request, sizeof(request));
}

syscall_status_t device_builder_finish_property(const struct device_builder* builder) {
	return builder_simple(builder, DEVICE_BUILDER_OP_FINISH_PROPERTY);
}

syscall_status_t device_builder_add_resource(const struct device_builder* builder, const char* name, size_t name_size,
                                             cap_id_t resource_cap, cap_rights_t rights) {
	struct device_builder_add_resource_request* request;
	cap_id_t                                    delegated = CAP_ID_INVALID;
	size_t                                      request_size;
	syscall_status_t                            status;

	if (builder == NULL || builder->capability == CAP_ID_INVALID || builder->manager_pid == PROCESS_PID_INVALID ||
	    !identifier_valid(name, name_size) || resource_cap == CAP_ID_INVALID || rights == 0u ||
	    name_size > UINT32_MAX || name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_delegate(resource_cap, builder->manager_pid, rights | CAP_DELEGATE, &delegated);
	if (status != SYSCALL_STATUS_OK) return status;
	request_size = sizeof(*request) + name_size;
	request      = calloc(1u, request_size);
	if (request == NULL) {
		(void)cap_revoke(delegated, 0u);
		return SYSCALL_STATUS_FAILED;
	}
	*request = (struct device_builder_add_resource_request){
		.header     = {.op = DEVICE_BUILDER_OP_ADD_RESOURCE},
		.name_size  = (uint32_t)name_size,
		.capability = delegated,
		.rights     = rights,
	};
	memcpy(request->name, name, name_size);
	status = call_empty(builder->capability, request, request_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) (void)cap_revoke(delegated, 0u);
	return status;
}

syscall_status_t device_builder_add_mmio_resource(const struct device_builder* builder, cap_id_t memory_allocator_cap,
                                                  const char* name, size_t name_size, uintptr_t physical_address,
                                                  size_t size) {
	static const cap_rights_t driver_rights = CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP;
	cap_id_t                  memory_cap    = CAP_ID_INVALID;
	syscall_status_t          status;
	syscall_status_t          cleanup_status;

	if (memory_allocator_cap == CAP_ID_INVALID || size == 0u) return SYSCALL_STATUS_BAD_ARGUMENT;
	status =
		memory_allocator_claim_physical(memory_allocator_cap, physical_address, size, MEMORY_TYPE_DEVICE, &memory_cap);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(builder, name, name_size, memory_cap, driver_rights);
	if (memory_cap != CAP_ID_INVALID) {
		cleanup_status = cap_drop(memory_cap);
		if (status == SYSCALL_STATUS_OK) status = cleanup_status;
	}
	return status;
}

syscall_status_t device_builder_commit(struct device_builder* builder) {
	syscall_status_t status;
	if (builder == NULL || builder->capability == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = builder_simple(builder, DEVICE_BUILDER_OP_COMMIT);
	if (status == SYSCALL_STATUS_OK)
		*builder = (struct device_builder){.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	return status;
}

syscall_status_t device_builder_abort(struct device_builder* builder) {
	syscall_status_t status;
	if (builder == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (builder->capability == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
	status = builder_simple(builder, DEVICE_BUILDER_OP_ABORT);
	if (status == SYSCALL_STATUS_OK || status == SYSCALL_STATUS_UNAVAILABLE)
		*builder = (struct device_builder){.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	return status;
}

syscall_status_t device_get_info(cap_id_t device_cap, struct device_info* out_info) {
	const struct device_object_simple_request request = {.header = {.op = DEVICE_OBJECT_OP_INFO}};
	if (out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = call_exact(device_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_info->name_size > DEVICE_NAME_MAX || out_info->compatible_count == 0u ||
	    out_info->compatible_count > DEVICE_MAX_COMPATIBLE_IDS || out_info->property_count > DEVICE_MAX_PROPERTIES ||
	    out_info->resource_count > DEVICE_MAX_RESOURCES)
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t read_index(cap_id_t cap, enum device_object_op op, uint64_t index, uint64_t offset,
                                   void* buffer, size_t size) {
	const struct device_object_read_index_request request = {
		.header = {.op = op},
		.index  = index,
		.offset = offset,
		.size   = size,
	};
	if ((size != 0u && buffer == NULL) || size > CAP_MAX_RESPONSE_SIZE) return SYSCALL_STATUS_BAD_ARGUMENT;
	return call_exact(cap, &request, sizeof(request), buffer, size);
}

syscall_status_t device_name_read(cap_id_t device_cap, uint64_t offset, void* buffer, size_t size) {
	return read_index(device_cap, DEVICE_OBJECT_OP_READ_NAME, 0u, offset, buffer, size);
}

syscall_status_t device_compatible_info(cap_id_t device_cap, uint64_t index, uint64_t* out_size) {
	const struct device_object_index_request request = {
		.header = {.op = DEVICE_OBJECT_OP_COMPATIBLE_INFO},
		.index  = index,
	};
	if (out_size == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = call_exact(device_cap, &request, sizeof(request), out_size, sizeof(*out_size));
	if (status == SYSCALL_STATUS_OK && (*out_size == 0u || *out_size > DEVICE_COMPATIBLE_ID_MAX))
		return SYSCALL_STATUS_FAILED;
	return status;
}

syscall_status_t device_compatible_read(cap_id_t device_cap, uint64_t index, uint64_t offset, void* buffer,
                                        size_t size) {
	return read_index(device_cap, DEVICE_OBJECT_OP_READ_COMPATIBLE, index, offset, buffer, size);
}

static syscall_status_t property_request(cap_id_t cap, enum device_object_op op, const char* name, size_t name_size,
                                         uint64_t offset, void* response, size_t response_size) {
	struct device_object_property_read_request* request;
	size_t                                      request_size;
	syscall_status_t                            status;

	if (cap == CAP_ID_INVALID || !identifier_valid(name, name_size) || name_size > UINT32_MAX ||
	    name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) || response_size > CAP_MAX_RESPONSE_SIZE ||
	    (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + name_size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = op;
	request->name_size = (uint32_t)name_size;
	request->offset    = offset;
	request->size      = response_size;
	memcpy(request->name, name, name_size);
	status = call_exact(cap, request, request_size, response, response_size);
	free(request);
	return status;
}

syscall_status_t device_property_get_info(cap_id_t device_cap, const char* name, size_t name_size,
                                          struct device_property_info* out_info) {
	struct device_object_property_info_request* request;
	size_t                                      request_size;
	syscall_status_t                            status;

	if (device_cap == CAP_ID_INVALID || !identifier_valid(name, name_size) || out_info == NULL ||
	    name_size > UINT32_MAX || name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + name_size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = DEVICE_OBJECT_OP_PROPERTY_INFO;
	request->name_size = (uint32_t)name_size;
	memcpy(request->name, name, name_size);
	status = call_exact(device_cap, request, request_size, out_info, sizeof(*out_info));
	free(request);
	if (status == SYSCALL_STATUS_OK && !property_shape_valid(out_info)) return SYSCALL_STATUS_FAILED;
	return status;
}

syscall_status_t device_property_read(cap_id_t device_cap, const char* name, size_t name_size, uint64_t offset,
                                      void* buffer, size_t size) {
	return property_request(device_cap, DEVICE_OBJECT_OP_READ_PROPERTY, name, name_size, offset, buffer, size);
}

static uint64_t read_u64_le(const uint8_t bytes[8]) {
	uint64_t value = 0u;
	for (size_t index = 0u; index < 8u; index++) value |= (uint64_t)bytes[index] << (index * 8u);
	return value;
}

static syscall_status_t scalar_info(cap_id_t cap, const char* name, enum device_property_type type,
                                    struct device_property_info* info) {
	syscall_status_t status;
	if (name == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = device_property_get_info(cap, name, strlen(name), info);
	if (status != SYSCALL_STATUS_OK) return status;
	return info->type == type && info->element_count == 1u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_BAD_ARGUMENT;
}

syscall_status_t device_property_read_bool(cap_id_t cap, const char* name, bool* out_value) {
	struct device_property_info info;
	uint8_t                     value;
	syscall_status_t            status;
	if (out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = scalar_info(cap, name, DEVICE_PROPERTY_BOOLEAN, &info);
	if (status != SYSCALL_STATUS_OK || info.value_size != 1u)
		return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
	status = device_property_read(cap, name, strlen(name), 0u, &value, 1u);
	if (status == SYSCALL_STATUS_OK && value > 1u) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) *out_value = value != 0u;
	return status;
}

static syscall_status_t read_integer(cap_id_t cap, const char* name, enum device_property_type type,
                                     uint64_t* out_value) {
	struct device_property_info info;
	uint8_t                     value[8];
	syscall_status_t            status;
	if (out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = scalar_info(cap, name, type, &info);
	if (status != SYSCALL_STATUS_OK || info.value_size != sizeof(value))
		return status == SYSCALL_STATUS_OK ? SYSCALL_STATUS_FAILED : status;
	status = device_property_read(cap, name, strlen(name), 0u, value, sizeof(value));
	if (status == SYSCALL_STATUS_OK) *out_value = read_u64_le(value);
	return status;
}

syscall_status_t device_property_read_u64(cap_id_t cap, const char* name, uint64_t* out_value) {
	return read_integer(cap, name, DEVICE_PROPERTY_UNSIGNED, out_value);
}

syscall_status_t device_property_read_i64(cap_id_t cap, const char* name, int64_t* out_value) {
	uint64_t raw;
	if (out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = read_integer(cap, name, DEVICE_PROPERTY_SIGNED, &raw);
	if (status == SYSCALL_STATUS_OK) memcpy(out_value, &raw, sizeof(raw));
	return status;
}

static syscall_status_t read_integer_array(cap_id_t cap, const char* name, enum device_property_type type,
                                           uint64_t first, uint64_t* values, size_t count) {
	struct device_property_info info;
	uint8_t*                    bytes;
	syscall_status_t            status;
	if (name == NULL || (count != 0u && values == NULL) || count > CAP_MAX_RESPONSE_SIZE / 8u ||
	    first > UINT64_MAX - count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = device_property_get_info(cap, name, strlen(name), &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.type != type || info.element_count > UINT64_MAX / 8u || first + count > info.element_count ||
	    info.value_size != info.element_count * 8u)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	bytes = malloc(count * 8u);
	if (bytes == NULL && count != 0u) return SYSCALL_STATUS_FAILED;
	status = device_property_read(cap, name, strlen(name), first * 8u, bytes, count * 8u);
	if (status == SYSCALL_STATUS_OK)
		for (size_t index = 0u; index < count; index++) values[index] = read_u64_le(bytes + index * 8u);
	free(bytes);
	return status;
}

syscall_status_t device_property_read_u64_array(cap_id_t cap, const char* name, uint64_t first, uint64_t* values,
                                                size_t count) {
	return read_integer_array(cap, name, DEVICE_PROPERTY_UNSIGNED_ARRAY, first, values, count);
}

syscall_status_t device_property_read_i64_array(cap_id_t cap, const char* name, uint64_t first, int64_t* values,
                                                size_t count) {
	uint64_t* temporary;
	if (count != 0u && values == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	temporary = malloc(count * sizeof(*temporary));
	if (temporary == NULL && count != 0u) return SYSCALL_STATUS_FAILED;
	syscall_status_t status = read_integer_array(cap, name, DEVICE_PROPERTY_SIGNED_ARRAY, first, temporary, count);
	if (status == SYSCALL_STATUS_OK)
		for (size_t index = 0u; index < count; index++)
			memcpy(&values[index], &temporary[index], sizeof(values[index]));
	free(temporary);
	return status;
}

syscall_status_t device_property_read_bool_array(cap_id_t cap, const char* name, uint64_t first, bool* values,
                                                 size_t count) {
	struct device_property_info info;
	uint8_t*                    bytes;
	syscall_status_t            status;
	if (name == NULL || (count != 0u && values == NULL) || count > CAP_MAX_RESPONSE_SIZE || first > UINT64_MAX - count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = device_property_get_info(cap, name, strlen(name), &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.type != DEVICE_PROPERTY_BOOLEAN_ARRAY || first + count > info.element_count ||
	    info.value_size != info.element_count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	bytes = malloc(count);
	if (bytes == NULL && count != 0u) return SYSCALL_STATUS_FAILED;
	status = device_property_read(cap, name, strlen(name), first, bytes, count);
	if (status == SYSCALL_STATUS_OK) {
		for (size_t index = 0u; index < count; index++) {
			if (bytes[index] > 1u) {
				status = SYSCALL_STATUS_FAILED;
				break;
			}
			values[index] = bytes[index] != 0u;
		}
	}
	free(bytes);
	return status;
}

static syscall_status_t read_property_all(cap_id_t cap, const char* name, void* buffer, size_t size) {
	uint8_t* target = buffer;
	for (size_t offset = 0u; offset < size;) {
		size_t chunk = size - offset;
		if (chunk > CAP_MAX_RESPONSE_SIZE) chunk = CAP_MAX_RESPONSE_SIZE;
		syscall_status_t status = device_property_read(cap, name, strlen(name), offset, target + offset, chunk);
		if (status != SYSCALL_STATUS_OK) return status;
		offset += chunk;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t device_property_read_string(cap_id_t cap, const char* name, char* buffer, size_t capacity,
                                             size_t* out_size) {
	struct device_property_info info;
	syscall_status_t            status;
	if (name == NULL || out_size == NULL || (capacity != 0u && buffer == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = device_property_get_info(cap, name, strlen(name), &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.type != DEVICE_PROPERTY_UTF8_STRING || info.value_size > SIZE_MAX) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_size = (size_t)info.value_size;
	if (buffer == NULL && capacity == 0u) return SYSCALL_STATUS_OK;
	if (capacity <= info.value_size) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = read_property_all(cap, name, buffer, (size_t)info.value_size);
	if (status == SYSCALL_STATUS_OK && !utf8_valid(buffer, (size_t)info.value_size)) return SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK) buffer[info.value_size] = '\0';
	return status;
}

syscall_status_t device_property_read_string_at(cap_id_t cap, const char* name, uint64_t index, char* buffer,
                                                size_t capacity, size_t* out_size) {
	struct device_property_info info;
	uint64_t                    offset = 0u;
	uint8_t                     length_bytes[4];
	syscall_status_t            status;
	if (name == NULL || out_size == NULL || (capacity != 0u && buffer == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = device_property_get_info(cap, name, strlen(name), &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.type != DEVICE_PROPERTY_UTF8_STRING_ARRAY || index >= info.element_count)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	for (uint64_t current = 0u; current <= index; current++) {
		uint32_t length;
		if (offset > info.value_size || info.value_size - offset < sizeof(length_bytes)) return SYSCALL_STATUS_FAILED;
		status = device_property_read(cap, name, strlen(name), offset, length_bytes, sizeof(length_bytes));
		if (status != SYSCALL_STATUS_OK) return status;
		length = (uint32_t)length_bytes[0] | ((uint32_t)length_bytes[1] << 8u) | ((uint32_t)length_bytes[2] << 16u) |
		         ((uint32_t)length_bytes[3] << 24u);
		offset += 4u;
		if (length > info.value_size - offset) return SYSCALL_STATUS_FAILED;
		if (current != index) {
			offset += length;
			continue;
		}
		*out_size = length;
		if (buffer == NULL && capacity == 0u) return SYSCALL_STATUS_OK;
		if (capacity <= length) return SYSCALL_STATUS_BAD_ARGUMENT;
		for (size_t copied = 0u; copied < length;) {
			size_t chunk = length - copied;
			if (chunk > CAP_MAX_RESPONSE_SIZE) chunk = CAP_MAX_RESPONSE_SIZE;
			status = device_property_read(cap, name, strlen(name), offset + copied, buffer + copied, chunk);
			if (status != SYSCALL_STATUS_OK) return status;
			copied += chunk;
		}
		if (!utf8_valid(buffer, length)) return SYSCALL_STATUS_FAILED;
		buffer[length] = '\0';
		return SYSCALL_STATUS_OK;
	}
	return SYSCALL_STATUS_FAILED;
}

syscall_status_t device_property_read_bytes(cap_id_t cap, const char* name, uint64_t offset, void* buffer,
                                            size_t size) {
	struct device_property_info info;
	if (name == NULL || (size != 0u && buffer == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = device_property_get_info(cap, name, strlen(name), &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.type != DEVICE_PROPERTY_BYTES || offset > info.value_size || size > info.value_size - offset)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	return device_property_read(cap, name, strlen(name), offset, buffer, size);
}

syscall_status_t device_resource_info(cap_id_t cap, uint64_t index, struct device_resource_info* out_info) {
	const struct device_object_index_request request = {
		.header = {.op = DEVICE_OBJECT_OP_RESOURCE_INFO},
		.index  = index,
	};
	if (out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = call_exact(cap, &request, sizeof(request), out_info, sizeof(*out_info));
	if (status == SYSCALL_STATUS_OK &&
	    (out_info->name_size == 0u || out_info->name_size > DEVICE_IDENTIFIER_MAX || out_info->rights == 0u))
		return SYSCALL_STATUS_FAILED;
	return status;
}

syscall_status_t device_resource_name_read(cap_id_t cap, uint64_t index, uint64_t offset, void* buffer, size_t size) {
	return read_index(cap, DEVICE_OBJECT_OP_READ_RESOURCE_NAME, index, offset, buffer, size);
}

syscall_status_t device_resource_acquire(cap_id_t cap, const char* name, size_t name_size, cap_rights_t rights,
                                         cap_id_t* out_cap) {
	struct device_object_resource_acquire_request* request;
	struct device_object_resource_acquire_response response = {.capability = CAP_ID_INVALID};
	size_t                                         request_size;
	syscall_status_t                               status;
	if (!identifier_valid(name, name_size) || out_cap == NULL || rights == 0u || name_size > UINT32_MAX ||
	    name_size > CAP_MAX_REQUEST_SIZE - sizeof(*request))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_cap     = CAP_ID_INVALID;
	request_size = sizeof(*request) + name_size;
	request      = calloc(1u, request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = DEVICE_OBJECT_OP_ACQUIRE_RESOURCE;
	request->name_size = (uint32_t)name_size;
	request->rights    = rights;
	memcpy(request->name, name, name_size);
	status = call_exact(cap, request, request_size, &response, sizeof(response));
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_cap = response.capability;
	return SYSCALL_STATUS_OK;
}
