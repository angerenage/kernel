#include "supervisor.h"

#include <protocol/filesystem.h>
#include <protocol/loader.h>
#include <protocol/terminal.h>
#include <protocol/text_input.h>
#include <protocol/tty_session.h>
#include <protocol/vfs.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/display.h>
#include <system/kernel_resource.h>
#include <system/process.h>
#include <system/signal.h>

#include "configuration.h"
#include "device_manager.h"
#include "launcher.h"
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

static process_id_t supervisor_pid = PROCESS_PID_INVALID;

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
#define TTY_SESSION_PROTOCOL_VERSION_MAJOR 1u
#define TEXT_INPUT_PROTOCOL_VERSION_MAJOR 1u

static enum vfs_readiness check_vfs_readiness(const struct registry_service_selector* selector) {
	const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle  root    = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result  result;
	enum registry_status           registry_status;
	syscall_status_t               cleanup_status = SYSCALL_STATUS_OK;

	(void)selector;
	registry_status = registry_acquire(&query, VFS_SERVICE_NAME, &service).status;
	if (registry_status == REGISTRY_STATUS_NOT_FOUND) return VFS_READINESS_WAIT;
	if (registry_status != REGISTRY_STATUS_OK) {
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
	const struct registry_protocol_query query = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.minor          = TERMINAL_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle terminal   = {.capability = CAP_ID_INVALID};
	cap_id_t                       output_cap = CAP_ID_INVALID;
	enum registry_status           status;

	status = registry_acquire(&query, TERMINAL_SERVICE_NAME, &terminal).status;
	if (status != REGISTRY_STATUS_OK) {
		printf("init: terminal output acquisition failed: %u\n", (unsigned)status);
		return false;
	}
	if (cap_delegate(terminal.capability, supervisor_pid, TERMINAL_OUTPUT_RIGHTS, &output_cap) != SYSCALL_STATUS_OK ||
	    cap_drop(terminal.capability) != SYSCALL_STATUS_OK) {
		if (output_cap != CAP_ID_INVALID) (void)cap_drop(output_cap);
		printf("init: terminal output rights reduction failed\n");
		return false;
	}
	display_set_standard_streams(CAP_ID_INVALID, output_cap, output_cap);
	serial_stream_disable();
	return true;
}

static enum bootfs_mount_readiness mount_bootfs(const struct registry_service_selector* selector) {
	const struct registry_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	const struct registry_protocol_query bootfs_query = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.minor          = FILESYSTEM_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle vfs_service    = {.capability = CAP_ID_INVALID};
	struct registry_service_handle bootfs_service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle  root           = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result  result;
	enum registry_status           registry_status;
	cap_id_t                       delegated_root = CAP_ID_INVALID;
	syscall_status_t               status;
	bool                           cleanup_ok = true;
	bool                           mounted    = false;

	(void)selector;
	registry_status = registry_acquire(&vfs_query, VFS_SERVICE_NAME, &vfs_service).status;
	if (registry_status != REGISTRY_STATUS_OK) {
		printf("init: VFS acquisition for bootfs mount failed: %u\n", (unsigned)registry_status);
		return BOOTFS_MOUNT_FAILED;
	}
	registry_status = registry_acquire(&bootfs_query, BOOTFS_SERVICE_NAME, &bootfs_service).status;
	if (registry_status == REGISTRY_STATUS_NOT_FOUND) {
		if (cap_drop(vfs_service.capability) != SYSCALL_STATUS_OK) return BOOTFS_MOUNT_FAILED;
		return BOOTFS_MOUNT_WAIT;
	}
	if (registry_status != REGISTRY_STATUS_OK) {
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
	const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle service = {.capability = CAP_ID_INVALID};
	enum registry_status           registry_status;
	enum init_configuration_status configuration_status;
	syscall_status_t               status;
	bool                           success = true;

	registry_status = registry_acquire(&query, VFS_SERVICE_NAME, &service).status;
	if (registry_status != REGISTRY_STATUS_OK) {
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

static bool service_available(const struct registry_service_selector* selector) {
	struct registry_protocol_query query = {
		.major = selector->major,
	};
	struct registry_service_handle service = {.capability = CAP_ID_INVALID};
	struct registry_call_result    result;

	(void)strlcpy(query.namespace_path, selector->namespace_path, sizeof(query.namespace_path));
	(void)strlcpy(query.protocol, selector->protocol, sizeof(query.protocol));
	result = registry_acquire(&query, selector->service, &service);
	if (result.status != REGISTRY_STATUS_OK) return false;
	return cap_drop(service.capability) == SYSCALL_STATUS_OK;
}

static bool watch_service(cap_id_t activity, const struct registry_service_selector* selector) {
	struct registry_protocol_query query = {
		.major = selector->major,
	};
	struct registry_call_result result;
	uint64_t                    subscription_id;
	uint64_t                    counter;

	(void)strlcpy(query.namespace_path, selector->namespace_path, sizeof(query.namespace_path));
	(void)strlcpy(query.protocol, selector->protocol, sizeof(query.protocol));
	result = registry_watch(&query, activity, &subscription_id, &counter);
	return result.status == REGISTRY_STATUS_OK && result.transport_status == SYSCALL_STATUS_OK;
}

int supervisor_run(const struct init_state* init) {
	struct init_configuration                     configuration;
	static const struct registry_service_selector vfs_selector = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.service        = VFS_SERVICE_NAME,
	};
	static const struct registry_service_selector bootfs_selector = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.service        = BOOTFS_SERVICE_NAME,
	};
	static const struct registry_service_selector terminal_selector = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.service        = TERMINAL_SERVICE_NAME,
	};
	static const struct registry_service_selector text_input_selector = {
		.namespace_path = TEXT_INPUT_NAMESPACE,
		.protocol       = TEXT_INPUT_PROTOCOL_NAME,
		.major          = TEXT_INPUT_PROTOCOL_VERSION_MAJOR,
		.service        = TEXT_INPUT_SERVICE_NAME,
	};
	static const struct registry_service_selector tty_session_selector = {
		.namespace_path = TTY_SESSION_NAMESPACE,
		.protocol       = TTY_SESSION_PROTOCOL_NAME,
		.major          = TTY_SESSION_PROTOCOL_VERSION_MAJOR,
		.service        = TTY_SESSION_SERVICE_NAME,
	};
	static const struct registry_service_selector loader_selector = {
		.namespace_path = PROGRAM_LOADER_NAMESPACE,
		.protocol       = LOADER_PROTOCOL_NAME,
		.major          = 1u,
		.service        = "elf64",
	};
	bool             bootfs_mounted              = false;
	bool             bootfs_started              = false;
	bool             configuration_loaded        = false;
	bool             device_manager_started      = false;
	bool             terminal_started            = false;
	bool             terminal_ready              = false;
	bool             terminal_available          = false;
	bool             input_manager_started       = false;
	bool             keyboard_controller_started = false;
	bool             text_input_started          = false;
	bool             tty_session_started         = false;
	bool             tty_session_ready           = false;
	bool             shell_started               = false;
	bool             loader_started              = false;
	bool             vfs_ready                   = false;
	bool             vfs_started                 = false;
	const char*      text_input_layout_override_path;
	const char*      text_input_layout = NULL;
	struct self_info self;
	cap_id_t         registry_activity = CAP_ID_INVALID;
	syscall_status_t status;

	if (init == NULL || init->registry_cap == CAP_ID_INVALID) return 1;
	if (!text_input_layout_override(init, &text_input_layout_override_path)) {
		printf("init: invalid or duplicate %s boot option\n", TEXT_INPUT_LAYOUT_OPTION);
		return 1;
	}
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return 1;
	supervisor_pid = self.pid;
	status         = signal_create(&registry_activity);
	if (status != SYSCALL_STATUS_OK || !watch_service(registry_activity, &vfs_selector) ||
	    !watch_service(registry_activity, &bootfs_selector) || !watch_service(registry_activity, &terminal_selector) ||
	    !watch_service(registry_activity, &tty_session_selector) ||
	    !watch_service(registry_activity, &text_input_selector) || !watch_service(registry_activity, &loader_selector))
		return 1;
	for (;;) {
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
			terminal_started   = true;
			terminal_ready     = !terminal_launched;
			terminal_available = terminal_launched;
			continue;
		}
		if (terminal_started && !terminal_ready && service_available(&terminal_selector)) {
			if (!use_terminal_output()) return 1;
			terminal_ready = true;
			continue;
		}
		if (terminal_ready && !tty_session_started) {
			if (!terminal_available) {
				tty_session_started = true;
				tty_session_ready   = true;
				continue;
			}
			if (!bootstrap_launch_terminal_session(init, "tty_session.elf", "TTY session", 0u, NULL, 0u, NULL))
				return 1;
			tty_session_started = true;
			continue;
		}
		if (tty_session_started && !tty_session_ready && service_available(&tty_session_selector)) {
			tty_session_ready = true;
			continue;
		}
		if (tty_session_ready && !configuration_loaded) {
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
		if (device_manager_started && !shell_started) {
			if (!terminal_available) {
				shell_started = true;
				continue;
			}
			if (service_available(&text_input_selector) && service_available(&tty_session_selector)) {
				if (!bootstrap_launch_interactive(init, "shell.elf", "shell", 0u, NULL, 0u, NULL)) return 1;
				shell_started = true;
				continue;
			}
		}
		struct signal_message activity;
		status = signal_wait(registry_activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}
