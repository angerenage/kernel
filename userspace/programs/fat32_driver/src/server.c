#include "server.h"

#include <base/channel.h>
#include <base/process.h>
#include <protocol/filesystem.h>
#include <runtime/device.h>
#include <runtime/filesystem.h>
#include <runtime/registry.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/trace.h>

#include "directory.h"
#include "write.h"

#define FAT32_FILESYSTEM_VERSION_MAJOR 1u
#define FAT32_FILESYSTEM_VERSION_MINOR 0u
#define FAT32_ROOT_OBJECT_ID 1u
#define FAT32_ATTRIBUTE_READ_ONLY 0x01u
#define FAT32_ATTRIBUTE_DIRECTORY 0x10u
#define FAT32_MAX_SERVICE_FILE_SIZE (16u * 1024u * 1024u)

struct fat32_node {
	struct fat32_directory_entry entry;
	uint32_t                     parent_cluster;
	struct fat32_node*           next;
};

struct fat32_read_cache {
	uint8_t* data;
	size_t   capacity;
	uint64_t offset;
	size_t   size;
	uint32_t file_cluster;
	uint32_t file_size;
	bool     valid;
	bool     unavailable;
};

struct fat32_server {
	struct fat32_volume*    volume;
	channel_id_t            endpoint;
	cap_id_t                activity_cap;
	cap_id_t                root_cap;
	process_id_t            pid;
	char                    service[REGISTRY_NAME_MAX + 1u];
	struct fat32_node*      nodes;
	struct fat32_read_cache read_cache;
};

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];
static uint8_t response_buffer[CAP_MAX_RESPONSE_SIZE];

TRACE_LABEL(trace_file_read, "fat32/file/read");
TRACE_LABEL(trace_cache_fill, "fat32/file/cache_fill");

static bool reply_request(const struct cap_request* call, const void* response, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, response, size, status) == SYSCALL_STATUS_OK;
}

static bool reply_transport(const struct cap_request* call, syscall_status_t status) {
	return reply_request(call, NULL, 0u, status);
}

static bool reply_domain(const struct cap_request* call, enum filesystem_status status) {
	const struct filesystem_response_header response = {.status = status, .reserved = 0u};
	return reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool request_has_rights(const struct cap_request* call, cap_rights_t rights) {
	return (call->rights & rights) == rights;
}

static bool copy_request(const struct cap_request* call, const void* data, void* output, size_t size) {
	if (data == NULL || output == NULL || call->request_size < size) return false;
	memcpy(output, data, size);
	return true;
}

static enum filesystem_status fat_status(enum fat32_result result) {
	switch (result) {
	case FAT32_RESULT_OK:
		return FILESYSTEM_STATUS_OK;
	case FAT32_RESULT_NOT_FOUND:
		return FILESYSTEM_STATUS_NOT_FOUND;
	case FAT32_RESULT_UNSUPPORTED:
		return FILESYSTEM_STATUS_UNSUPPORTED;
	case FAT32_RESULT_ERROR:
	default:
		return FILESYSTEM_STATUS_IO_ERROR;
	}
}

static bool node_is_directory(const struct fat32_node* node) {
	return node == NULL || (node->entry.attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u;
}

static uint32_t node_cluster(const struct fat32_server* server, const struct fat32_node* node) {
	return node == NULL ? server->volume->root_cluster : node->entry.cluster;
}

static bool node_read_only(const struct fat32_server* server, const struct fat32_node* node) {
	return (server->volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u ||
	       (node != NULL && (node->entry.attributes & FAT32_ATTRIBUTE_READ_ONLY) != 0u);
}

static struct filesystem_node_info node_info(const struct fat32_server* server, const struct fat32_node* node) {
	return (struct filesystem_node_info){
		.type  = node_is_directory(node) ? FILESYSTEM_NODE_DIRECTORY : FILESYSTEM_NODE_FILE,
		.flags = node_read_only(server, node) ? FILESYSTEM_NODE_READ_ONLY : 0u,
		.size  = node == NULL || node_is_directory(node) ? 0u : node->entry.size,
	};
}

static uint64_t node_object_id(const struct fat32_node* node) {
	return (uint64_t)(uintptr_t)node;
}

static struct fat32_node* find_node(struct fat32_server* server, uint64_t object_id) {
	for (struct fat32_node* node = server->nodes; node != NULL; node = node->next)
		if (node_object_id(node) == object_id) return node;
	return NULL;
}

static void unlink_node(struct fat32_server* server, struct fat32_node* target) {
	struct fat32_node** cursor = &server->nodes;
	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor = target->next;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static void invalidate_read_cache(struct fat32_server* server) {
	server->read_cache.offset       = 0u;
	server->read_cache.size         = 0u;
	server->read_cache.file_cluster = 0u;
	server->read_cache.file_size    = 0u;
	server->read_cache.valid        = false;
}

static bool initialize_read_cache(struct fat32_server* server) {
	struct fat32_read_cache* cache = &server->read_cache;
	if (cache->data != NULL) return true;
	if (cache->unavailable || server->volume->buffer.size <= CAP_MAX_RESPONSE_SIZE) return false;
	cache->capacity = server->volume->buffer.size;
	cache->data     = malloc(cache->capacity);
	if (cache->data != NULL) return true;
	cache->capacity    = 0u;
	cache->unavailable = true;
	return false;
}

static enum fat32_result read_file(struct fat32_server* server, struct fat32_node* node, uint64_t offset, void* output,
                                   size_t size, size_t* out_read) {
	struct fat32_read_cache* cache = &server->read_cache;
	TRACE_SCOPE(trace, &trace_file_read);
	if (!initialize_read_cache(server))
		return fat32_file_read(server->volume, &node->entry, offset, output, size, out_read);
	*out_read = 0u;
	if (size == 0u || offset >= node->entry.size) return FAT32_RESULT_OK;
	bool cache_hit = cache->valid && cache->file_cluster == node->entry.cluster &&
	                 cache->file_size == node->entry.size && offset >= cache->offset &&
	                 offset - cache->offset < cache->size;
	if (!cache_hit) {
		size_t            read;
		enum fat32_result result;
		TRACE_SCOPE(fill_trace, &trace_cache_fill);
		invalidate_read_cache(server);
		result = fat32_file_read(server->volume, &node->entry, offset, cache->data, cache->capacity, &read);
		if (result != FAT32_RESULT_OK) return result;
		cache->offset       = offset;
		cache->size         = read;
		cache->file_cluster = node->entry.cluster;
		cache->file_size    = node->entry.size;
		cache->valid        = true;
	}
	size_t cache_offset = (size_t)(offset - cache->offset);
	size_t copied       = cache->size - cache_offset;
	if (copied > size) copied = size;
	memcpy(output, cache->data + cache_offset, copied);
	*out_read = copied;
	return FAT32_RESULT_OK;
}

static enum fat32_result refresh_node(struct fat32_server* server, struct fat32_node* node) {
	return fat32_directory_lookup(
		server->volume, node->parent_cluster, node->entry.name, strlen(node->entry.name), &node->entry);
}

static enum fat32_result refresh_matching_nodes(struct fat32_server* server, const struct fat32_node* changed) {
	struct fat32_directory_entry entry;
	enum fat32_result            result = fat32_directory_lookup(
		server->volume, changed->parent_cluster, changed->entry.name, strlen(changed->entry.name), &entry);
	if (result != FAT32_RESULT_OK) return result;
	for (struct fat32_node* node = server->nodes; node != NULL; node = node->next)
		if (node->parent_cluster == changed->parent_cluster && strcmp(node->entry.name, changed->entry.name) == 0)
			node->entry = entry;
	return FAT32_RESULT_OK;
}

static bool publish_node(struct fat32_server* server, const struct cap_request* call,
                         const struct fat32_directory_entry* entry, uint32_t parent_cluster, cap_rights_t rights) {
	struct filesystem_open_response response;
	struct fat32_node*              node;
	syscall_status_t                status;

	node = calloc(1u, sizeof(*node));
	if (node == NULL) return reply_transport(call, SYSCALL_STATUS_FAILED);
	node->entry          = *entry;
	node->parent_cluster = parent_cluster;
	if (node_object_id(node) == 0u || node_object_id(node) == FAT32_ROOT_OBJECT_ID) {
		free(node);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	status = cap_derive(server->root_cap, call->caller, node_object_id(node), CAP_CALL | rights, &response.capability);
	if (status != SYSCALL_STATUS_OK) {
		free(node);
		return reply_transport(call, status);
	}
	node->next      = server->nodes;
	server->nodes   = node;
	response.header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	response.info   = node_info(server, node);
	if (reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(server->endpoint, node_object_id(node));
	unlink_node(server, node);
	free(node);
	return false;
}

static enum fat32_result lookup_path(struct fat32_server* server, uint32_t start_cluster, const uint8_t* path,
                                     size_t path_size, struct fat32_directory_entry* out_entry,
                                     uint32_t* out_parent_cluster) {
	uint32_t cluster = start_cluster;
	size_t   start   = 0u;
	for (;;) {
		size_t end = start;
		while (end < path_size && path[end] != '/') end++;
		enum fat32_result result =
			fat32_directory_lookup(server->volume, cluster, path + start, end - start, out_entry);
		if (result != FAT32_RESULT_OK) return result;
		if (end == path_size) {
			*out_parent_cluster = cluster;
			return FAT32_RESULT_OK;
		}
		if ((out_entry->attributes & FAT32_ATTRIBUTE_DIRECTORY) == 0u) return FAT32_RESULT_NOT_FOUND;
		cluster = out_entry->cluster;
		start   = end + 1u;
	}
}

static bool handle_info(struct fat32_server* server, const struct cap_request* call, const void* data,
                        struct fat32_node* node) {
	struct filesystem_info_request request;
	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(struct filesystem_info_response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	const struct filesystem_info_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = node_info(server, node),
	};
	return reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_open(struct fat32_server* server, const struct cap_request* call, const void* data,
                        struct fat32_node* directory) {
	struct filesystem_directory_open_request request;
	struct fat32_directory_entry             entry;
	uint32_t                                 parent_cluster;
	size_t                                   expected_size;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) ||
	    (request.rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) != 0u || (request.rights & ~call->rights) != 0u)
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	const uint8_t* path = (const uint8_t*)data + sizeof(request);
	if (!filesystem_relative_path_valid(path, request.path_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	enum fat32_result result =
		lookup_path(server, node_cluster(server, directory), path, request.path_size, &entry, &parent_cluster);
	if (result != FAT32_RESULT_OK) return reply_domain(call, fat_status(result));
	if ((request.rights & CAP_WRITE) != 0u && ((server->volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u ||
	                                           (entry.attributes & FAT32_ATTRIBUTE_READ_ONLY) != 0u))
		return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	return publish_node(server, call, &entry, parent_cluster, request.rights);
}

static void fill_entry(struct fat32_server* server, struct filesystem_directory_entry* output,
                       const struct fat32_directory_entry* input) {
	size_t name_size = strlen(input->name);
	*output          = (struct filesystem_directory_entry){
		.name_size = (uint32_t)name_size,
		.info =
			{
				   .type  = (input->attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u ? FILESYSTEM_NODE_DIRECTORY
				                                                               : FILESYSTEM_NODE_FILE,
				   .flags = ((server->volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u ||
				          (input->attributes & FAT32_ATTRIBUTE_READ_ONLY) != 0u)
				             ? FILESYSTEM_NODE_READ_ONLY
				             : 0u,
				   .size  = (input->attributes & FAT32_ATTRIBUTE_DIRECTORY) != 0u ? 0u : input->size,
				   },
	};
	memcpy(output->name, input->name, name_size);
}

static bool handle_enumerate(struct fat32_server* server, const struct cap_request* call, const void* data,
                             struct fat32_node* directory) {
	struct filesystem_directory_enumerate_request   request;
	struct filesystem_directory_enumerate_response* response = (void*)response_buffer;
	size_t maximum = (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(struct filesystem_directory_entry);
	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(*response) || request.count > maximum ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(struct filesystem_directory_entry))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	struct fat32_directory_entry* entries =
		request.count == 0u ? NULL : malloc((size_t)request.count * sizeof(*entries));
	if (request.count != 0u && entries == NULL) return reply_transport(call, SYSCALL_STATUS_FAILED);
	uint64_t          total;
	size_t            returned;
	enum fat32_result result = fat32_directory_enumerate(server->volume,
	                                                     node_cluster(server, directory),
	                                                     request.offset,
	                                                     entries,
	                                                     (size_t)request.count,
	                                                     &returned,
	                                                     &total);
	if (result != FAT32_RESULT_OK) {
		free(entries);
		return reply_domain(call, fat_status(result));
	}
	for (size_t index = 0u; index < returned; index++) fill_entry(server, &response->entries[index], &entries[index]);
	free(entries);
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	response->total    = total;
	response->returned = returned;
	return reply_request(
		call, response, sizeof(*response) + returned * sizeof(struct filesystem_directory_entry), SYSCALL_STATUS_OK);
}

static bool handle_create_file(struct fat32_server* server, const struct cap_request* call, const void* data,
                               struct fat32_node* directory) {
	struct filesystem_directory_open_request request;
	struct fat32_directory_entry             entry;
	size_t                                   expected_size;
	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) ||
	    (request.rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) != 0u || (request.rights & ~call->rights) != 0u)
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	const uint8_t* path = (const uint8_t*)data + sizeof(request);
	if (!filesystem_relative_path_valid(path, request.path_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (directory != NULL || memchr(path, '/', request.path_size) != NULL ||
	    request.path_size >= FAT32_SHORT_NAME_CAPACITY)
		return reply_domain(call, FILESYSTEM_STATUS_UNSUPPORTED);
	char name[FAT32_SHORT_NAME_CAPACITY];
	memcpy(name, path, request.path_size);
	name[request.path_size] = '\0';
	enum fat32_result result =
		fat32_directory_lookup(server->volume, server->volume->root_cluster, path, request.path_size, &entry);
	if (result == FAT32_RESULT_OK) return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	if (result != FAT32_RESULT_NOT_FOUND) return reply_domain(call, fat_status(result));
	result = fat32_write_root_file(server->volume, name, NULL, 0u);
	if (result != FAT32_RESULT_OK) return reply_domain(call, fat_status(result));
	result = fat32_directory_lookup(server->volume, server->volume->root_cluster, path, request.path_size, &entry);
	if (result != FAT32_RESULT_OK) return reply_domain(call, fat_status(result));
	return publish_node(server, call, &entry, server->volume->root_cluster, request.rights);
}

static bool handle_file_read(struct fat32_server* server, const struct cap_request* call, const void* data,
                             struct fat32_node* node) {
	struct filesystem_file_read_request   request;
	struct filesystem_file_read_response* response = (void*)response_buffer;
	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE - sizeof(*response) ||
	    call->response_capacity < sizeof(*response) || request.size > call->response_capacity - sizeof(*response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	size_t            read;
	enum fat32_result result = read_file(server, node, request.offset, response->data, (size_t)request.size, &read);
	if (result != FAT32_RESULT_OK) return reply_domain(call, fat_status(result));
	response->header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	return reply_request(call, response, sizeof(*response) + read, SYSCALL_STATUS_OK);
}

static enum fat32_result replace_file(struct fat32_server* server, struct fat32_node* node, uint64_t offset,
                                      const void* data, size_t size, uint64_t final_size) {
	if (node->parent_cluster != server->volume->root_cluster || final_size > FAT32_MAX_SERVICE_FILE_SIZE ||
	    offset > final_size || size > final_size - offset)
		return FAT32_RESULT_UNSUPPORTED;
	enum fat32_result result = refresh_node(server, node);
	if (result != FAT32_RESULT_OK) return result;
	uint8_t* contents = final_size == 0u ? NULL : calloc(1u, (size_t)final_size);
	if (final_size != 0u && contents == NULL) return FAT32_RESULT_ERROR;
	size_t preserve = node->entry.size < final_size ? node->entry.size : (size_t)final_size;
	if (preserve != 0u) {
		size_t read;
		result = fat32_file_read(server->volume, &node->entry, 0u, contents, preserve, &read);
		if (result != FAT32_RESULT_OK || read != preserve) result = FAT32_RESULT_ERROR;
	}
	if (result == FAT32_RESULT_OK && size != 0u) memcpy(contents + offset, data, size);
	if (result == FAT32_RESULT_OK)
		result = fat32_write_root_file(server->volume, node->entry.name, contents, (size_t)final_size);
	free(contents);
	if (result == FAT32_RESULT_OK) {
		invalidate_read_cache(server);
		result = refresh_matching_nodes(server, node);
	}
	return result;
}

static bool handle_file_write(struct fat32_server* server, const struct cap_request* call, const void* data,
                              struct fat32_node* node) {
	struct filesystem_file_write_request request;
	size_t                               expected_size;
	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.reserved != 0u ||
	    request.size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + (size_t)request.size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_file_write_response) ||
	    request.offset > UINT64_MAX - request.size)
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	uint64_t final_size = node->entry.size;
	if (request.size != 0u && request.offset + request.size > final_size) final_size = request.offset + request.size;
	enum fat32_result result = replace_file(
		server, node, request.offset, (const uint8_t*)data + sizeof(request), (size_t)request.size, final_size);
	if (result != FAT32_RESULT_OK) return reply_domain(call, fat_status(result));
	const struct filesystem_file_write_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.size   = request.size,
	};
	return reply_request(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_file_resize(struct fat32_server* server, const struct cap_request* call, const void* data,
                               struct fat32_node* node) {
	struct filesystem_file_resize_request request;
	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	enum fat32_result result = replace_file(server, node, 0u, NULL, 0u, request.size);
	return result == FAT32_RESULT_OK ? reply_domain(call, FILESYSTEM_STATUS_OK)
	                                 : reply_domain(call, fat_status(result));
}

static bool dispatch_directory(struct fat32_server* server, const struct cap_request* call, const void* data,
                               const struct filesystem_request_header* header, struct fat32_node* node) {
	switch (header->op) {
	case FILESYSTEM_DIRECTORY_OP_INFO:
		return handle_info(server, call, data, node);
	case FILESYSTEM_DIRECTORY_OP_OPEN:
		return handle_open(server, call, data, node);
	case FILESYSTEM_DIRECTORY_OP_ENUMERATE:
		return handle_enumerate(server, call, data, node);
	case FILESYSTEM_DIRECTORY_OP_CREATE_FILE:
		return handle_create_file(server, call, data, node);
	case FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY:
	case FILESYSTEM_DIRECTORY_OP_REMOVE:
	case FILESYSTEM_DIRECTORY_OP_RENAME:
		if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
		return reply_domain(call, FILESYSTEM_STATUS_UNSUPPORTED);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_file(struct fat32_server* server, const struct cap_request* call, const void* data,
                          const struct filesystem_request_header* header, struct fat32_node* node) {
	switch (header->op) {
	case FILESYSTEM_FILE_OP_INFO:
		return handle_info(server, call, data, node);
	case FILESYSTEM_FILE_OP_READ:
		return handle_file_read(server, call, data, node);
	case FILESYSTEM_FILE_OP_WRITE:
		return handle_file_write(server, call, data, node);
	case FILESYSTEM_FILE_OP_RESIZE:
		return handle_file_resize(server, call, data, node);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_request(struct fat32_server* server, const struct cap_request* call, const void* data) {
	struct filesystem_request_header header;
	struct fat32_node*               node = NULL;
	if (!request_has_rights(call, CAP_CALL)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &header, sizeof(header))) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->object_id != FAT32_ROOT_OBJECT_ID) {
		node = find_node(server, call->object_id);
		if (node == NULL) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
	return node_is_directory(node) ? dispatch_directory(server, call, data, &header, node)
	                               : dispatch_file(server, call, data, &header, node);
}

static bool service_name(struct fat32_server* server, cap_id_t device_cap) {
	if (server->volume->volume_id != 0u) {
		int length = snprintf(server->service,
		                      sizeof(server->service),
		                      "%04x-%04x",
		                      server->volume->volume_id >> 16u,
		                      server->volume->volume_id & 0xffffu);
		return length > 0 && (size_t)length < sizeof(server->service);
	}
	struct device_info info;
	if (device_get_info(device_cap, &info) != SYSCALL_STATUS_OK || info.name_size == 0u ||
	    info.name_size >= sizeof(server->service) ||
	    device_name_read(device_cap, 0u, server->service, (size_t)info.name_size) != SYSCALL_STATUS_OK)
		return false;
	server->service[info.name_size] = '\0';
	for (size_t index = 0u; index < info.name_size; index++) {
		char value = server->service[index];
		if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
		      value == '_' || value == '-' || value == '.'))
			server->service[index] = '-';
	}
	return true;
}

static bool advertise(struct fat32_server* server, cap_id_t device_cap) {
	struct self_info self;
	if (!service_name(server, device_cap) || process_self_info(&self) != SYSCALL_STATUS_OK ||
	    self.pid == PROCESS_PID_INVALID)
		return false;
	server->pid = self.pid;
	if (channel_create(&server->endpoint, &server->activity_cap) != SYSCALL_STATUS_OK) return false;
	cap_rights_t client_rights = CAP_CALL | CAP_READ | CAP_DELEGATE;
	if ((server->volume->device.flags & BLOCK_DEVICE_WRITABLE) != 0u) client_rights |= CAP_WRITE;
	if (cap_publish(
			server->endpoint, FAT32_ROOT_OBJECT_ID, server->pid, client_rights | CAP_MANAGE, &server->root_cap) !=
	    SYSCALL_STATUS_OK)
		return false;
	const struct registry_service_selector selector = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FAT32_FILESYSTEM_VERSION_MAJOR,
	};
	struct registry_service_selector named = selector;
	(void)strlcpy(named.service, server->service, sizeof(named.service));
	struct registry_call_result result =
		registry_advertise(&named, FAT32_FILESYSTEM_VERSION_MINOR, server->root_cap, client_rights);
	return result.transport_status == SYSCALL_STATUS_OK && result.status == REGISTRY_STATUS_OK;
}

static void handle_event(struct fat32_server* server, const struct channel_event* event) {
	if (event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->object_id == FAT32_ROOT_OBJECT_ID) return;
	struct fat32_node* node = find_node(server, event->object_id);
	if (node == NULL || cap_unpublish_if_unused(server->endpoint, event->object_id) != SYSCALL_STATUS_OK) return;
	unlink_node(server, node);
	free(node);
}

enum fat32_result fat32_server_run(struct fat32_volume* volume, cap_id_t device_cap) {
	struct fat32_server server = {
		.volume       = volume,
		.endpoint     = CHANNEL_ID_INVALID,
		.activity_cap = CAP_ID_INVALID,
		.root_cap     = CAP_ID_INVALID,
		.pid          = PROCESS_PID_INVALID,
	};
	if (!advertise(&server, device_cap)) return FAT32_RESULT_ERROR;
	printf("fat32: advertised %s/%s@%u.%u/%s\n",
	       FILESYSTEM_NAMESPACE,
	       FILESYSTEM_PROTOCOL_NAME,
	       FAT32_FILESYSTEM_VERSION_MAJOR,
	       FAT32_FILESYSTEM_VERSION_MINOR,
	       server.service);
	for (;;) {
		bool received;
		do {
			struct cap_request call;
			received = false;
			if (channel_recv(server.endpoint, &call, request_buffer, sizeof(request_buffer), &received) !=
			    SYSCALL_STATUS_OK)
				return FAT32_RESULT_ERROR;
			if (received && !dispatch_request(&server, &call, request_buffer)) return FAT32_RESULT_ERROR;
		} while (received);
		do {
			struct channel_event event;
			received = false;
			if (channel_event_recv(server.endpoint, &event, &received) != SYSCALL_STATUS_OK) return FAT32_RESULT_ERROR;
			if (received) handle_event(&server, &event);
		} while (received);
		struct signal_message activity;
		if (signal_wait(server.activity_cap, &activity) != SYSCALL_STATUS_OK) return FAT32_RESULT_ERROR;
	}
}
