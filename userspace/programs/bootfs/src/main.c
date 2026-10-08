#include <base/cap.h>
#include <base/module.h>
#include <base/process.h>
#include <protocol/filesystem.h>
#include <runtime/filesystem.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/module.h>
#include <system/process.h>
#include <system/signal.h>

#define BOOTFS_SERVICE_NAME "boot"
#define BOOTFS_ROOT_OBJECT_ID 1u
#define BOOTFS_BOOT_OBJECT_ID 2u
#define BOOTFS_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_DELEGATE))
#define BOOTFS_SOURCE_RIGHTS ((cap_rights_t)(BOOTFS_CLIENT_RIGHTS | CAP_MANAGE))

struct bootfs_file {
	cap_id_t                            module_cap;
	const struct module_provider_entry* module;
	struct bootfs_file*                 next;
};

static channel_id_t                  bootfs_endpoint  = CHANNEL_ID_INVALID;
static cap_id_t                      bootfs_activity  = CAP_ID_INVALID;
static cap_id_t                      bootfs_root_cap  = CAP_ID_INVALID;
static cap_id_t                      modules_provider = CAP_ID_INVALID;
static struct module_provider_entry* modules;
static size_t                        module_count;
static struct bootfs_file*           files;
static uint8_t                       request_buffer[CAP_MAX_REQUEST_SIZE];
static uint8_t                       response_buffer[CAP_MAX_RESPONSE_SIZE];

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool reply_transport(const struct cap_request* call, syscall_status_t status) {
	return reply_request(call->call_id, NULL, 0u, status);
}

static bool reply_domain(const struct cap_request* call, enum filesystem_status status) {
	const struct filesystem_response_header response = {
		.status   = status,
		.reserved = 0u,
	};
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool request_has_rights(const struct cap_request* call, cap_rights_t rights) {
	return (call->rights & rights) == rights;
}

static bool copy_request(const struct cap_request* call, const void* data, void* out, size_t size) {
	if (call == NULL || data == NULL || out == NULL || call->request_size < size) return false;
	memcpy(out, data, size);
	return true;
}

static bool node_rights_valid(cap_rights_t rights) {
	return (rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) == 0u;
}

static uint64_t file_object_id(const struct bootfs_file* file) {
	return (uint64_t)(uintptr_t)file;
}

static struct bootfs_file* find_file(uint64_t object_id) {
	for (struct bootfs_file* file = files; file != NULL; file = file->next) {
		if (file_object_id(file) == object_id) return file;
	}
	return NULL;
}

static void unlink_file(struct bootfs_file* target) {
	struct bootfs_file** cursor = &files;
	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor      = target->next;
			target->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static void release_file(struct bootfs_file* file) {
	if (file == NULL) return;
	if (file->module_cap != CAP_ID_INVALID) (void)cap_drop(file->module_cap);
	free(file);
}

static bool module_name_valid(const struct module_provider_entry* module) {
	const char* terminator;
	size_t      name_size;

	if (module == NULL) return false;
	terminator = memchr(module->name, '\0', sizeof(module->name));
	if (terminator == NULL) return false;
	name_size = (size_t)(terminator - module->name);
	return name_size != 0u && filesystem_relative_path_valid(module->name, name_size) &&
	       memchr(module->name, '/', name_size) == NULL;
}

static bool module_name_unique(size_t index) {
	for (size_t i = 0u; i < index; i++) {
		if (strcmp(modules[i].name, modules[index].name) == 0) return false;
	}
	return true;
}

static bool load_module_descriptors(void) {
	uint64_t         total = 0u;
	size_t           returned;
	syscall_status_t status;

	status = module_enumerate(modules_provider, 0u, NULL, 0u, &returned, &total);
	if (status != SYSCALL_STATUS_OK || returned != 0u || total > SIZE_MAX / sizeof(*modules)) return false;
	if (total == 0u) return true;
	modules = calloc((size_t)total, sizeof(*modules));
	if (modules == NULL) return false;
	while (module_count < (size_t)total) {
		uint64_t current_total = 0u;
		status                 = module_enumerate(modules_provider,
		                                          module_count,
		                                          modules + module_count,
		                                          (size_t)total - module_count,
		                                          &returned,
		                                          &current_total);
		if (status != SYSCALL_STATUS_OK || returned == 0u || current_total != total) return false;
		for (size_t i = 0u; i < returned; i++) {
			size_t index = module_count + i;
			if (!module_name_valid(&modules[index]) || !module_name_unique(index)) return false;
		}
		module_count += returned;
	}
	return true;
}

static const struct module_provider_entry* find_module(const void* name, size_t name_size) {
	for (size_t i = 0u; i < module_count; i++) {
		size_t candidate_size = strlen(modules[i].name);
		if (candidate_size == name_size && memcmp(modules[i].name, name, name_size) == 0) return &modules[i];
	}
	return NULL;
}

static struct filesystem_node_info directory_info(void) {
	return (struct filesystem_node_info){
		.type  = FILESYSTEM_NODE_DIRECTORY,
		.flags = FILESYSTEM_NODE_READ_ONLY,
		.size  = 0u,
	};
}

static struct filesystem_node_info file_info(const struct module_provider_entry* module) {
	return (struct filesystem_node_info){
		.type  = FILESYSTEM_NODE_FILE,
		.flags = FILESYSTEM_NODE_READ_ONLY,
		.size  = module->size,
	};
}

static bool reply_open_directory(const struct cap_request* call, cap_rights_t rights) {
	struct filesystem_open_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = directory_info(),
	};
	syscall_status_t status =
		cap_derive(bootfs_root_cap, call->caller, BOOTFS_BOOT_OBJECT_ID, CAP_CALL | rights, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply_transport(call, status);
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool reply_open_file(const struct cap_request* call, const struct module_provider_entry* module,
                            cap_rights_t rights) {
	struct module_provider_resolve_response resolved = {.cap = CAP_ID_INVALID};
	struct filesystem_open_response         response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = file_info(module),
	};
	struct bootfs_file* file;
	syscall_status_t    status;

	status = module_resolve(modules_provider, module->name, strlen(module->name) + 1u, &resolved);
	if (status != SYSCALL_STATUS_OK) return reply_transport(call, status);
	if (resolved.id != module->id || resolved.size != module->size) {
		(void)cap_drop(resolved.cap);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	file = calloc(1u, sizeof(*file));
	if (file == NULL) {
		(void)cap_drop(resolved.cap);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	file->module_cap = resolved.cap;
	file->module     = module;
	if (file_object_id(file) == 0u || file_object_id(file) == BOOTFS_ROOT_OBJECT_ID ||
	    file_object_id(file) == BOOTFS_BOOT_OBJECT_ID) {
		release_file(file);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	status = cap_derive(bootfs_root_cap, call->caller, file_object_id(file), CAP_CALL | rights, &response.capability);
	if (status != SYSCALL_STATUS_OK) {
		release_file(file);
		return reply_transport(call, status);
	}
	file->next = files;
	files      = file;
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(bootfs_endpoint, file_object_id(file));
	unlink_file(file);
	release_file(file);
	return false;
}

static bool handle_directory_info(const struct cap_request* call, const void* data) {
	struct filesystem_info_request  request;
	struct filesystem_info_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = directory_info(),
	};

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_directory_open(const struct cap_request* call, const void* data, bool root) {
	struct filesystem_directory_open_request request;
	const struct module_provider_entry*      module = NULL;
	const uint8_t*                           path;
	size_t                                   expected_size;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) || !node_rights_valid(request.rights))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	path = (const uint8_t*)data + sizeof(request);
	if (!filesystem_relative_path_valid(path, request.path_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if ((request.rights & CAP_WRITE) != 0u) return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	if (root && request.path_size == sizeof("boot") - 1u && memcmp(path, "boot", sizeof("boot") - 1u) == 0)
		return reply_open_directory(call, request.rights);
	if (root && request.path_size > sizeof("boot/") - 1u && memcmp(path, "boot/", sizeof("boot/") - 1u) == 0) {
		path += sizeof("boot/") - 1u;
		request.path_size -= sizeof("boot/") - 1u;
	}
	else if (root) {
		return reply_domain(call, FILESYSTEM_STATUS_NOT_FOUND);
	}
	module = find_module(path, request.path_size);
	if (module == NULL) return reply_domain(call, FILESYSTEM_STATUS_NOT_FOUND);
	return reply_open_file(call, module, request.rights);
}

static void fill_directory_entry(struct filesystem_directory_entry* entry, const char* name,
                                 struct filesystem_node_info info) {
	size_t name_size = strlen(name);
	*entry           = (struct filesystem_directory_entry){
		.name_size = (uint32_t)name_size,
		.reserved  = 0u,
		.info      = info,
	};
	memcpy(entry->name, name, name_size);
}

static bool handle_directory_enumerate(const struct cap_request* call, const void* data, bool root) {
	struct filesystem_directory_enumerate_request   request;
	struct filesystem_directory_enumerate_response* response = (void*)response_buffer;
	const size_t                                    maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(struct filesystem_directory_entry);
	uint64_t total    = root ? 1u : module_count;
	size_t   returned = 0u;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(*response) || request.count > maximum_count ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(struct filesystem_directory_entry))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.offset < total) {
		uint64_t available = total - request.offset;
		returned           = available > request.count ? (size_t)request.count : (size_t)available;
	}
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	response->total    = total;
	response->returned = returned;
	for (size_t i = 0u; i < returned; i++) {
		if (root) {
			fill_directory_entry(&response->entries[i], "boot", directory_info());
		}
		else {
			const struct module_provider_entry* module = &modules[(size_t)request.offset + i];
			fill_directory_entry(&response->entries[i], module->name, file_info(module));
		}
	}
	return reply_request(call->call_id,
	                     response,
	                     sizeof(*response) + returned * sizeof(struct filesystem_directory_entry),
	                     SYSCALL_STATUS_OK);
}

static bool dispatch_directory(const struct cap_request* call, const void* data,
                               const struct filesystem_request_header* header, bool root) {
	switch (header->op) {
	case FILESYSTEM_DIRECTORY_OP_INFO:
		return handle_directory_info(call, data);
	case FILESYSTEM_DIRECTORY_OP_OPEN:
		return handle_directory_open(call, data, root);
	case FILESYSTEM_DIRECTORY_OP_ENUMERATE:
		return handle_directory_enumerate(call, data, root);
	case FILESYSTEM_DIRECTORY_OP_CREATE_FILE:
	case FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY:
	case FILESYSTEM_DIRECTORY_OP_REMOVE:
	case FILESYSTEM_DIRECTORY_OP_RENAME:
		if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
		return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool handle_file_info(const struct cap_request* call, const void* data, const struct bootfs_file* file) {
	struct filesystem_info_request  request;
	struct filesystem_info_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = file_info(file->module),
	};

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_file_read(const struct cap_request* call, const void* data, const struct bootfs_file* file) {
	struct filesystem_file_read_request   request;
	struct filesystem_file_read_response* response  = (void*)response_buffer;
	size_t                                read_size = 0u;
	syscall_status_t                      status;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE - sizeof(*response) ||
	    call->response_capacity < sizeof(*response) || request.size > call->response_capacity - sizeof(*response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.offset < file->module->size) {
		uint64_t available = file->module->size - request.offset;
		read_size          = available > request.size ? (size_t)request.size : (size_t)available;
	}
	response->header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	if (read_size != 0u) {
		status = module_read(file->module_cap, request.offset, response->data, read_size);
		if (status != SYSCALL_STATUS_OK) return reply_transport(call, status);
	}
	return reply_request(call->call_id, response, sizeof(*response) + read_size, SYSCALL_STATUS_OK);
}

static bool dispatch_file(const struct cap_request* call, const void* data,
                          const struct filesystem_request_header* header, const struct bootfs_file* file) {
	switch (header->op) {
	case FILESYSTEM_FILE_OP_INFO:
		return handle_file_info(call, data, file);
	case FILESYSTEM_FILE_OP_READ:
		return handle_file_read(call, data, file);
	case FILESYSTEM_FILE_OP_WRITE:
	case FILESYSTEM_FILE_OP_RESIZE:
		if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
		return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_request(const struct cap_request* call, const void* data) {
	struct filesystem_request_header header;
	struct bootfs_file*              file;

	if (!request_has_rights(call, CAP_CALL)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &header, sizeof(header))) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->object_id == BOOTFS_ROOT_OBJECT_ID) return dispatch_directory(call, data, &header, true);
	if (call->object_id == BOOTFS_BOOT_OBJECT_ID) return dispatch_directory(call, data, &header, false);
	file = find_file(call->object_id);
	if (file == NULL) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return dispatch_file(call, data, &header, file);
}

static bool advertise_bootfs(void) {
	const struct init_service_selector selector = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.service        = BOOTFS_SERVICE_NAME,
	};
	struct init_call_result result;
	struct self_info        self;
	syscall_status_t        status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	status = channel_create(&bootfs_endpoint, &bootfs_activity);
	if (status != SYSCALL_STATUS_OK) return false;
	status = cap_publish(bootfs_endpoint, BOOTFS_ROOT_OBJECT_ID, self.pid, BOOTFS_SOURCE_RIGHTS, &bootfs_root_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	result = init_advertise(&selector, FILESYSTEM_PROTOCOL_VERSION_MINOR, bootfs_root_cap, BOOTFS_CLIENT_RIGHTS);
	return result.status == INIT_REGISTRY_OK && result.transport_status == SYSCALL_STATUS_OK;
}

static void handle_channel_event(const struct channel_event* event) {
	struct bootfs_file* file;

	if (event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->object_id == BOOTFS_ROOT_OBJECT_ID ||
	    event->object_id == BOOTFS_BOOT_OBJECT_ID)
		return;
	file = find_file(event->object_id);
	if (file == NULL || cap_unpublish_if_unused(bootfs_endpoint, event->object_id) != SYSCALL_STATUS_OK) return;
	unlink_file(file);
	release_file(file);
}

static int bootfs_run(void) {
	struct cap_request call;
	bool               received;
	syscall_status_t   status;

	if (!load_module_descriptors()) {
		printf("bootfs: module enumeration failed\n");
		return 1;
	}
	if (!advertise_bootfs()) {
		printf("bootfs: advertisement failed\n");
		return 1;
	}
	printf("bootfs: advertised %s/%s@%u.%u/%s\n",
	       FILESYSTEM_NAMESPACE,
	       FILESYSTEM_PROTOCOL_NAME,
	       FILESYSTEM_PROTOCOL_VERSION_MAJOR,
	       FILESYSTEM_PROTOCOL_VERSION_MINOR,
	       BOOTFS_SERVICE_NAME);
	for (;;) {
		do {
			received = false;
			status   = channel_recv(bootfs_endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) {
				printf("bootfs: channel receive failed: %u\n", (unsigned)status);
				return 1;
			}
			if (received && !dispatch_request(&call, request_buffer)) printf("bootfs: channel reply failed\n");
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(bootfs_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received) handle_channel_event(&event);
		} while (received);
		struct signal_message activity;
		status = signal_wait(bootfs_activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	if (capc != 1u || capv == NULL || capv[0] == CAP_ID_INVALID) return 1;
	modules_provider = capv[0];
	return bootfs_run();
}
