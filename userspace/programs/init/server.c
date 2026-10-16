#include "server.h"

#include <protocol/filesystem.h>
#include <protocol/terminal.h>
#include <protocol/vfs.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/display.h>
#include <system/kernel_resource.h>
#include <system/process.h>
#include <system/signal.h>

#include "configuration.h"
#include "device_manager.h"
#include "launcher.h"
#include "registry.h"
#include "serial_stream.h"

#define BOOTFS_SERVICE_NAME "boot"
#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u
#define FILESYSTEM_PROTOCOL_VERSION_MAJOR 1u
#define FILESYSTEM_PROTOCOL_VERSION_MINOR 0u
#define TEXT_INPUT_LAYOUT_OPTION "text_input.layout"
#define TEXT_INPUT_LAYOUT_QWERTY "qwerty"
#define TEXT_INPUT_LAYOUT_AZERTY "azerty"
#define TEXT_INPUT_QWERTY_PATH "/boot/keyboard_qwerty.xml"
#define TEXT_INPUT_AZERTY_PATH "/boot/keyboard_azerty.xml"
#define SYSTEM_CONFIGURATION_PATH "/boot/system.yaml"

static channel_id_t server_endpoint = CHANNEL_ID_INVALID;
static cap_id_t     server_activity = CAP_ID_INVALID;
static process_id_t server_pid      = PROCESS_PID_INVALID;

enum vfs_readiness {
	VFS_READINESS_WAIT,
	VFS_READINESS_READY,
	VFS_READINESS_FAILED,
};

enum bootfs_mount_readiness {
	BOOTFS_MOUNT_WAIT,
	BOOTFS_MOUNT_READY,
	BOOTFS_MOUNT_FAILED,
};

#define TERMINAL_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_PROTOCOL_VERSION_MINOR 0u

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size,
                          syscall_status_t response_status) {
	syscall_status_t status = channel_reply(call_id, response, response_size, response_status);
	if (status == SYSCALL_STATUS_OK) return true;
	printf("init: channel reply failed: %u\n", (unsigned)status);
	return false;
}

static enum vfs_readiness check_vfs_readiness(const struct init_service_selector* selector) {
	const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle root    = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	enum init_registry_status     registry_status;
	syscall_status_t              cleanup_status = SYSCALL_STATUS_OK;

	if (!registry_contains(selector)) return VFS_READINESS_WAIT;
	registry_status = registry_acquire(server_pid, &query, VFS_SERVICE_NAME, &service);
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: VFS readiness acquisition failed: %u\n", (unsigned)registry_status);
		return VFS_READINESS_FAILED;
	}
	result = vfs_open(service.capability, "/", 1u, CAP_READ, &root);
	if (root.capability != CAP_ID_INVALID && cap_drop(root.capability) != SYSCALL_STATUS_OK)
		cleanup_status = SYSCALL_STATUS_FAILED;
	if (cap_drop(service.capability) != SYSCALL_STATUS_OK) cleanup_status = SYSCALL_STATUS_FAILED;
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK ||
	    root.info.type != FILESYSTEM_NODE_DIRECTORY || cleanup_status != SYSCALL_STATUS_OK) {
		printf("init: VFS readiness check failed: transport=%u status=%u\n",
		       (unsigned)result.transport_status,
		       (unsigned)result.status);
		return VFS_READINESS_FAILED;
	}
	return VFS_READINESS_READY;
}

static bool launch_bootfs(const struct init_state* init) {
	struct program_capability_argument argument;
	cap_id_t                           modules_cap = CAP_ID_INVALID;
	syscall_status_t                   status;
	bool                               launched;

	status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_MODULES, &modules_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: bootfs modules provider acquisition failed: %u\n", (unsigned)status);
		return false;
	}
	argument = (struct program_capability_argument){
		.capability = modules_cap,
		.rights     = CAP_CALL | CAP_READ,
	};
	launched = bootstrap_launch(init, "bootfs.elf", "bootfs", 0u, NULL, 1u, &argument);
	status   = cap_drop(modules_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: bootfs modules provider capability drop failed: %u\n", (unsigned)status);
		return false;
	}
	return launched;
}

static bool launch_terminal(const struct init_state* init, bool* out_launched) {
	struct program_capability_argument argument;
	cap_id_t                           framebuffer_cap = CAP_ID_INVALID;
	syscall_status_t                   status;
	bool                               launched;

	if (out_launched == NULL) return false;
	*out_launched = false;
	status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_FRAMEBUFFER, &framebuffer_cap);
	if (status == SYSCALL_STATUS_UNAVAILABLE) return true;
	if (status != SYSCALL_STATUS_OK) {
		printf("init: framebuffer acquisition failed: %u\n", (unsigned)status);
		return false;
	}
	argument = (struct program_capability_argument){
		.capability = framebuffer_cap,
		.rights     = CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP,
	};
	launched = bootstrap_launch(init, "terminal.elf", "terminal", 0u, NULL, 1u, &argument);
	if (launched) *out_launched = true;
	status = cap_drop(framebuffer_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: framebuffer capability drop failed: %u\n", (unsigned)status);
		return false;
	}
	return launched;
}

static bool use_terminal_output(void) {
	const struct init_protocol_query query = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.minor          = TERMINAL_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle terminal = {.capability = CAP_ID_INVALID};
	enum init_registry_status  status;

	status = registry_acquire(server_pid, &query, TERMINAL_SERVICE_NAME, &terminal);
	if (status != INIT_REGISTRY_OK) {
		printf("init: terminal output acquisition failed: %u\n", (unsigned)status);
		return false;
	}
	display_set_standard_streams(CAP_ID_INVALID, terminal.capability, terminal.capability);
	serial_stream_disable();
	return true;
}

static enum bootfs_mount_readiness mount_bootfs(const struct init_service_selector* selector) {
	const struct init_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	const struct init_protocol_query bootfs_query = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.minor          = FILESYSTEM_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    vfs_service    = {.capability = CAP_ID_INVALID};
	struct init_service_handle    bootfs_service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle root           = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	enum init_registry_status     registry_status;
	cap_id_t                      delegated_root = CAP_ID_INVALID;
	syscall_status_t              status;
	bool                          cleanup_ok = true;
	bool                          mounted    = false;

	if (!registry_contains(selector)) return BOOTFS_MOUNT_WAIT;
	registry_status = registry_acquire(server_pid, &vfs_query, VFS_SERVICE_NAME, &vfs_service);
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: VFS acquisition for bootfs mount failed: %u\n", (unsigned)registry_status);
		return BOOTFS_MOUNT_FAILED;
	}
	registry_status = registry_acquire(server_pid, &bootfs_query, BOOTFS_SERVICE_NAME, &bootfs_service);
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: bootfs acquisition failed: %u\n", (unsigned)registry_status);
		goto cleanup;
	}
	result = vfs_open(vfs_service.capability, "/", 1u, CAP_READ | CAP_MANAGE, &root);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK ||
	    root.info.type != FILESYSTEM_NODE_DIRECTORY) {
		printf("init: VFS root open for bootfs mount failed: transport=%u status=%u\n",
		       (unsigned)result.transport_status,
		       (unsigned)result.status);
		goto cleanup;
	}
	status =
		cap_delegate(bootfs_service.capability, vfs_service.owner, CAP_CALL | CAP_READ | CAP_DELEGATE, &delegated_root);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: bootfs root delegation failed: %u\n", (unsigned)status);
		goto cleanup;
	}
	result = vfs_directory_mount(root.capability, delegated_root, VFS_MOUNT_READ_ONLY);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK) {
		printf("init: bootfs mount failed: transport=%u status=%u\n",
		       (unsigned)result.transport_status,
		       (unsigned)result.status);
		status = cap_revoke(delegated_root, 0u);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: bootfs delegated root revocation failed: %u\n", (unsigned)status);
			cleanup_ok = false;
		}
		goto cleanup;
	}
	mounted = true;

cleanup:
	if (root.capability != CAP_ID_INVALID && cap_drop(root.capability) != SYSCALL_STATUS_OK) cleanup_ok = false;
	if (bootfs_service.capability != CAP_ID_INVALID && cap_drop(bootfs_service.capability) != SYSCALL_STATUS_OK)
		cleanup_ok = false;
	if (vfs_service.capability != CAP_ID_INVALID && cap_drop(vfs_service.capability) != SYSCALL_STATUS_OK)
		cleanup_ok = false;
	if (!mounted || !cleanup_ok) return BOOTFS_MOUNT_FAILED;
	printf("init: mounted bootfs at /\n");
	return BOOTFS_MOUNT_READY;
}

static bool text_input_layout_override(const struct init_state* init, const char** out_path) {
	static const char prefix[] = TEXT_INPUT_LAYOUT_OPTION "=";
	const char*       selected = NULL;
	const char*       argument;

	if (init == NULL || out_path == NULL) return false;
	*out_path = NULL;
	argument  = init->argv_data;
	for (uint32_t index = 0u; index < init->argc; index++) {
		if (strcmp(argument, TEXT_INPUT_LAYOUT_OPTION) == 0) return false;
		if (strncmp(argument, prefix, sizeof(prefix) - 1u) == 0) {
			if (selected != NULL) return false;
			selected = argument + sizeof(prefix) - 1u;
		}
		argument += strlen(argument) + 1u;
	}
	if (selected == NULL) return true;
	if (strcmp(selected, TEXT_INPUT_LAYOUT_QWERTY) == 0) {
		*out_path = TEXT_INPUT_QWERTY_PATH;
		return true;
	}
	if (strcmp(selected, TEXT_INPUT_LAYOUT_AZERTY) == 0) {
		*out_path = TEXT_INPUT_AZERTY_PATH;
		return true;
	}
	return false;
}

static bool load_system_configuration(struct init_configuration* configuration) {
	const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle     service = {.capability = CAP_ID_INVALID};
	enum init_registry_status      registry_status;
	enum init_configuration_status configuration_status;
	syscall_status_t               status;
	bool                           success = true;

	registry_status = registry_acquire(server_pid, &query, VFS_SERVICE_NAME, &service);
	if (registry_status != INIT_REGISTRY_OK) {
		printf("init: VFS acquisition for system configuration failed: %u\n", (unsigned)registry_status);
		return false;
	}
	configuration_status = init_configuration_load(service.capability, SYSTEM_CONFIGURATION_PATH, configuration);
	status               = cap_drop(service.capability);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: VFS capability drop after system configuration failed: %u\n", (unsigned)status);
		success = false;
	}
	if (configuration_status != INIT_CONFIGURATION_OK) {
		printf("init: system configuration load failed: %s\n", init_configuration_status_string(configuration_status));
		success = false;
	}
	return success;
}

bool server_init(void) {
	struct self_info self;
	syscall_status_t status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) {
		printf("init: self process query failed: %u\n", (unsigned)status);
		return false;
	}
	status = channel_create(&server_endpoint, &server_activity);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: service channel creation failed: %u\n", (unsigned)status);
		return false;
	}
	server_pid = self.pid;
	return true;
}

void server_deinit(void) {
	if (server_endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(server_endpoint);
	server_endpoint = CHANNEL_ID_INVALID;
	server_activity = CAP_ID_INVALID;
	server_pid      = PROCESS_PID_INVALID;
}

syscall_status_t init_server_grant(process_id_t target, cap_id_t* out_cap) {
	if (server_endpoint == CHANNEL_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return cap_publish(
		server_endpoint, INIT_SERVICE_OBJECT_ID, target, CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER, out_cap);
}

static bool dispatch_request(const struct cap_request* request, const void* data) {
	const struct init_request_header* header = data;

	switch (header->op) {
	case INIT_OP_GET_INFO: {
		const struct init_get_info_response response = {
			.status   = INIT_REGISTRY_OK,
			.init_pid = server_pid,
		};
		if (request->request_size != sizeof(struct init_get_info_request) ||
		    request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_ADVERTISE: {
		const struct init_advertise_request* advertise = data;
		struct init_registry_response        response;
		if (request->request_size != sizeof(*advertise) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_advertise(
			request->caller, &advertise->selector, advertise->minor, advertise->capability, advertise->client_rights);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_WITHDRAW: {
		const struct init_withdraw_request* withdraw = data;
		struct init_registry_response       response;
		if (request->request_size != sizeof(*withdraw) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_withdraw(request->caller, &withdraw->selector);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_ACQUIRE: {
		const struct init_acquire_request* acquire  = data;
		struct init_acquire_response       response = {0};
		bool                               replied;
		if (request->request_size != sizeof(*acquire) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_acquire(request->caller, &acquire->query, acquire->service, &response.handle);
		replied         = reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
		if (!replied && response.status == INIT_REGISTRY_OK && response.handle.capability != CAP_ID_INVALID)
			(void)cap_revoke(response.handle.capability, 0u);
		return replied;
	}
	case INIT_OP_ENUMERATE: {
		const struct init_enumerate_request* enumerate = data;
		struct init_enumerate_response*      response;
		size_t                               capacity;
		size_t                               response_size;
		if (request->request_size != sizeof(*enumerate) || request->response_capacity < sizeof(*response) ||
		    enumerate->size > (request->response_capacity - sizeof(*response)) / sizeof(struct init_service_info))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)enumerate->size * sizeof(struct init_service_info);
		response = malloc(capacity);
		if (response == NULL) {
			const struct init_enumerate_response failure = {
				.status = INIT_REGISTRY_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_enumerate(&enumerate->query,
		                                        enumerate->offset,
		                                        enumerate->size,
		                                        response->entries,
		                                        &response->returned,
		                                        &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct init_service_info);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case INIT_OP_BROWSE: {
		const struct init_browse_request* browse = data;
		struct init_browse_response*      response;
		size_t                            capacity;
		size_t                            response_size;
		if (request->request_size != sizeof(*browse) || request->response_capacity < sizeof(*response) ||
		    browse->size > (request->response_capacity - sizeof(*response)) / sizeof(struct init_browse_entry))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		capacity = sizeof(*response) + (size_t)browse->size * sizeof(struct init_browse_entry);
		response = malloc(capacity);
		if (response == NULL) {
			const struct init_browse_response failure = {
				.status = INIT_REGISTRY_NO_MEMORY, .total = 0u, .returned = 0u};
			return reply_request(request->call_id, &failure, sizeof(failure), SYSCALL_STATUS_OK);
		}
		response->returned = 0u;
		response->total    = 0u;
		response->status   = registry_browse(browse->namespace_path,
		                                     browse->offset,
		                                     browse->size,
		                                     response->entries,
		                                     &response->returned,
		                                     &response->total);
		response_size      = sizeof(*response) + (size_t)response->returned * sizeof(struct init_browse_entry);
		bool replied       = reply_request(request->call_id, response, response_size, SYSCALL_STATUS_OK);
		free(response);
		return replied;
	}
	case INIT_OP_WATCH: {
		const struct init_watch_request* watch    = data;
		struct init_watch_response       response = {0};
		if (request->request_size != sizeof(*watch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_watch(
			request->caller, &watch->query, watch->signal_capability, &response.subscription_id, &response.counter);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	case INIT_OP_UNWATCH: {
		const struct init_unwatch_request* unwatch = data;
		struct init_registry_response      response;
		if (request->request_size != sizeof(*unwatch) || request->response_capacity < sizeof(response))
			return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
		response.status = registry_unwatch(request->caller, unwatch->subscription_id);
		return reply_request(request->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
	}
	default:
		return reply_request(request->call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

int server_run(const struct init_state* init) {
	struct init_configuration                 configuration;
	static const struct init_service_selector vfs_selector = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.service        = VFS_SERVICE_NAME,
	};
	static const struct init_service_selector bootfs_selector = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.service        = BOOTFS_SERVICE_NAME,
	};
	static const struct init_service_selector terminal_selector = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.service        = TERMINAL_SERVICE_NAME,
	};
	union {
		struct init_request_header    header;
		struct init_advertise_request advertise;
		struct init_withdraw_request  withdraw;
		struct init_acquire_request   acquire;
		struct init_enumerate_request enumerate;
		struct init_browse_request    browse;
		struct init_watch_request     watch;
		struct init_unwatch_request   unwatch;
	} buffer;
	struct cap_request request;
	bool               received;
	bool               bootfs_mounted              = false;
	bool               bootfs_started              = false;
	bool               configuration_loaded        = false;
	bool               device_manager_started      = false;
	bool               terminal_started            = false;
	bool               terminal_ready              = false;
	bool               input_manager_started       = false;
	bool               keyboard_controller_started = false;
	bool               text_input_started          = false;
	bool               loader_started              = false;
	bool               vfs_ready                   = false;
	bool               vfs_started                 = false;
	const char*        text_input_layout_override_path;
	const char*        text_input_layout = NULL;
	syscall_status_t   status;

	if (server_endpoint == CHANNEL_ID_INVALID || init == NULL) return 1;
	if (!text_input_layout_override(init, &text_input_layout_override_path)) {
		printf("init: invalid or duplicate %s boot option\n", TEXT_INPUT_LAYOUT_OPTION);
		return 1;
	}
	for (;;) {
		do {
			received = false;
			status   = channel_recv(server_endpoint, &request, &buffer, sizeof(buffer), &received);
			if (status != SYSCALL_STATUS_OK) {
				printf("init: channel receive failed: %u\n", (unsigned)status);
				return 1;
			}
			if (received) {
				if (request.object_id != INIT_SERVICE_OBJECT_ID || (request.rights & CAP_CALL) == 0u ||
				    request.request_size < sizeof(buffer.header)) {
					if (!reply_request(request.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return 1;
				}
				else if (!dispatch_request(&request, &buffer)) return 1;
			}
		} while (received);
		do {
			struct channel_event event;
			status = channel_event_recv(server_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
		} while (received);
		if (!vfs_started) {
			if (!bootstrap_launch(init, "vfs.elf", "VFS", 0u, NULL, 0u, NULL)) return 1;
			vfs_started = true;
			continue;
		}
		if (!vfs_ready) {
			enum vfs_readiness readiness = check_vfs_readiness(&vfs_selector);
			if (readiness == VFS_READINESS_FAILED) return 1;
			if (readiness == VFS_READINESS_READY) {
				vfs_ready = true;
				continue;
			}
		}
		if (vfs_ready && !bootfs_started) {
			if (!launch_bootfs(init)) return 1;
			bootfs_started = true;
			continue;
		}
		if (bootfs_started && !bootfs_mounted) {
			enum bootfs_mount_readiness readiness = mount_bootfs(&bootfs_selector);
			if (readiness == BOOTFS_MOUNT_FAILED) return 1;
			if (readiness == BOOTFS_MOUNT_READY) {
				bootfs_mounted = true;
				continue;
			}
		}
		if (bootfs_mounted && !terminal_started) {
			bool terminal_launched;
			if (!launch_terminal(init, &terminal_launched)) return 1;
			terminal_started = true;
			terminal_ready   = !terminal_launched;
			continue;
		}
		if (terminal_started && !terminal_ready && registry_contains(&terminal_selector)) {
			if (!use_terminal_output()) return 1;
			terminal_ready = true;
			continue;
		}
		if (terminal_ready && !configuration_loaded) {
			if (!load_system_configuration(&configuration)) return 1;
			text_input_layout    = text_input_layout_override_path == NULL ? configuration.text_input_layout
			                                                               : text_input_layout_override_path;
			configuration_loaded = true;
			continue;
		}
		if (configuration_loaded && !loader_started) {
			if (!bootstrap_launch(init, "loader.elf", "loader", 0u, NULL, 0u, NULL)) return 1;
			loader_started = true;
			continue;
		}
		if (loader_started && !input_manager_started) {
			if (!bootstrap_launch(init, "input_manager.elf", "input manager", 0u, NULL, 0u, NULL)) return 1;
			input_manager_started = true;
			continue;
		}
		if (input_manager_started && !keyboard_controller_started) {
			if (!bootstrap_launch(init, "keyboard_controller.elf", "keyboard controller", 0u, NULL, 0u, NULL)) return 1;
			keyboard_controller_started = true;
			continue;
		}
		if (keyboard_controller_started && !text_input_started) {
			const char* arguments[] = {text_input_layout};
			if (!bootstrap_launch(init, "text_input.elf", "text input", 1u, arguments, 0u, NULL)) return 1;
			text_input_started = true;
			continue;
		}
		if (text_input_started && !device_manager_started) {
			enum device_manager_launch_result result = device_manager_launch(init);

			if (result == DEVICE_MANAGER_LAUNCH_FAILED) return 1;
			if (result == DEVICE_MANAGER_LAUNCH_SUCCESS) {
				device_manager_started = true;
				continue;
			}
		}
		struct signal_message activity;
		status = signal_wait(server_activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}
