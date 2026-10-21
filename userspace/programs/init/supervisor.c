#include "supervisor.h"

#include <limits.h>
#include <protocol/filesystem.h>
#include <protocol/loader.h>
#include <protocol/terminal.h>
#include <protocol/text_input.h>
#include <protocol/tty_session.h>
#include <protocol/vfs.h>
#include <runtime/program.h>
#include <runtime/vfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/display.h>
#include <system/kernel_resource.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>
#include <system/time.h>

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
#define SUPERVISOR_RESTART_BASE_MS 250u
#define SUPERVISOR_RESTART_MAX_MS 4000u
#define SUPERVISOR_CONTROL_OBJECT_ID 1u

static process_id_t supervisor_pid = PROCESS_PID_INVALID;

enum supervised_process_id {
	SUPERVISED_VFS = 0u,
	SUPERVISED_BOOTFS,
	SUPERVISED_TERMINAL,
	SUPERVISED_TTY_SESSION,
	SUPERVISED_LOADER,
	SUPERVISED_INPUT_MANAGER,
	SUPERVISED_KEYBOARD_CONTROLLER,
	SUPERVISED_TEXT_INPUT,
	SUPERVISED_DEVICE_MANAGER,
	SUPERVISED_SHELL,
	SUPERVISED_PROCESS_COUNT,
};

enum supervised_process_state {
	SUPERVISED_PROCESS_STOPPED = 0u,
	SUPERVISED_PROCESS_STARTING,
	SUPERVISED_PROCESS_RUNNING,
	SUPERVISED_PROCESS_BACKOFF,
	SUPERVISED_PROCESS_DISABLED,
};

struct supervised_process {
	const char*                   description;
	enum supervised_process_state state;
	uint64_t                      generation;
	unsigned int                  restart_count;
	cap_id_t                      process_cap;
};

enum supervisor_event_op {
	SUPERVISOR_EVENT_PROCESS_EXIT = 0u,
	SUPERVISOR_EVENT_RETRY,
	SUPERVISOR_EVENT_REGISTRY_CHANGE,
};

struct supervisor_event {
	enum supervisor_event_op   op;
	enum supervised_process_id process;
	uint64_t                   generation;
	syscall_status_t           wait_status;
	uintptr_t                  exit_code;
};

struct process_monitor_argument {
	cap_id_t                   process_cap;
	cap_id_t                   control_cap;
	enum supervised_process_id process;
	uint64_t                   generation;
};

struct process_retry_argument {
	cap_id_t                   control_cap;
	enum supervised_process_id process;
	uint64_t                   generation;
	size_t                     delay_ms;
};

struct registry_monitor_argument {
	cap_id_t registry_activity;
	cap_id_t control_cap;
};

static void monitor_process(void* opaque) {
	const struct process_monitor_argument* argument = opaque;
	struct supervisor_event                event    = {
		.op         = SUPERVISOR_EVENT_PROCESS_EXIT,
		.process    = argument->process,
		.generation = argument->generation,
		.exit_code  = PROCESS_EXIT_SYSTEM_UNKNOWN,
	};

	event.wait_status = process_wait(argument->process_cap, &event.exit_code);
	thread_exit(cap_call(argument->control_cap, &event, sizeof(event), NULL, 0u, NULL) == SYSCALL_STATUS_OK ? 0u : 1u);
}

static void retry_process(void* opaque) {
	const struct process_retry_argument* argument = opaque;
	const struct supervisor_event        event    = {
		.op         = SUPERVISOR_EVENT_RETRY,
		.process    = argument->process,
		.generation = argument->generation,
	};

	(void)sleep_ms(argument->delay_ms);
	thread_exit(cap_call(argument->control_cap, &event, sizeof(event), NULL, 0u, NULL) == SYSCALL_STATUS_OK ? 0u : 1u);
}

static void monitor_registry(void* opaque) {
	const struct registry_monitor_argument* argument = opaque;
	const struct supervisor_event           event    = {.op = SUPERVISOR_EVENT_REGISTRY_CHANGE};
	struct signal_message                   message;

	for (;;) {
		if (signal_wait(argument->registry_activity, &message) != SYSCALL_STATUS_OK) thread_exit(1u);
		if (cap_call(argument->control_cap, &event, sizeof(event), NULL, 0u, NULL) != SYSCALL_STATUS_OK)
			thread_exit(1u);
	}
}

static bool spawn_worker(cap_id_t self_cap, uintptr_t entry, const void* argument, size_t argument_size) {
	cap_id_t         thread_cap = CAP_ID_INVALID;
	syscall_status_t status     = process_spawn_thread(self_cap, entry, argument, argument_size, NULL, 0u, &thread_cap);

	if (status == SYSCALL_STATUS_OK) status = thread_detach(thread_cap);
	if (thread_cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(thread_cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	return status == SYSCALL_STATUS_OK;
}

static void process_begin_launch(struct supervised_process* process) {
	process->generation++;
	if (process->generation == 0u) process->generation = 1u;
	process->state       = SUPERVISED_PROCESS_STARTING;
	process->process_cap = CAP_ID_INVALID;
}

static bool process_started(struct supervised_process* process, enum supervised_process_id id, cap_id_t self_cap,
                            cap_id_t control_cap, cap_id_t process_cap) {
	const struct process_monitor_argument argument = {
		.process_cap = process_cap,
		.control_cap = control_cap,
		.process     = id,
		.generation  = process->generation,
	};

	if (process_cap == CAP_ID_INVALID ||
	    !spawn_worker(self_cap, (uintptr_t)monitor_process, &argument, sizeof(argument))) {
		if (process_cap != CAP_ID_INVALID) {
			(void)process_kill(process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
			(void)process_wait(process_cap, NULL);
		}
		return false;
	}
	process->process_cap = process_cap;
	process->state       = SUPERVISED_PROCESS_RUNNING;
	return true;
}

static bool process_schedule_restart(struct supervised_process* process, enum supervised_process_id id,
                                     cap_id_t self_cap, cap_id_t control_cap) {
	size_t delay = SUPERVISOR_RESTART_BASE_MS;

	for (unsigned int retry = 0u; retry < process->restart_count && delay < SUPERVISOR_RESTART_MAX_MS; retry++)
		delay *= 2u;
	if (delay > SUPERVISOR_RESTART_MAX_MS) delay = SUPERVISOR_RESTART_MAX_MS;
	if (process->restart_count != UINT_MAX) process->restart_count++;
	process->state                               = SUPERVISED_PROCESS_BACKOFF;
	const struct process_retry_argument argument = {
		.control_cap = control_cap,
		.process     = id,
		.generation  = process->generation,
		.delay_ms    = delay,
	};
	printf("init: restarting %s in %llu ms\n", process->description, (unsigned long long)delay);
	return spawn_worker(self_cap, (uintptr_t)retry_process, &argument, sizeof(argument));
}

static bool process_finish_launch(struct supervised_process* process, enum supervised_process_id id, cap_id_t self_cap,
                                  cap_id_t control_cap, bool launched, cap_id_t process_cap) {
	if (launched && process_started(process, id, self_cap, control_cap, process_cap)) return true;
	if (!launched && process_cap != CAP_ID_INVALID) {
		(void)process_kill(process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		(void)process_wait(process_cap, NULL);
	}
	return process_schedule_restart(process, id, self_cap, control_cap);
}

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

static bool launch_bootfs(const struct init_state* init, cap_id_t* out_process_cap) {
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
	launched = bootstrap_launch(init, "bootfs.elf", "bootfs", 0u, NULL, 1u, &argument, out_process_cap);
	status   = cap_drop(modules_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: bootfs modules provider capability drop failed: %u\n", (unsigned)status);
		return false;
	}
	return launched;
}

static bool launch_terminal(const struct init_state* init, bool* out_launched, cap_id_t* out_process_cap) {
	struct program_capability_argument argument;
	cap_id_t                           framebuffer_cap = CAP_ID_INVALID;
	syscall_status_t                   status;
	bool                               launched;

	if (out_launched == NULL || out_process_cap == NULL || *out_process_cap != CAP_ID_INVALID) return false;
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
	launched = bootstrap_launch(init, "terminal.elf", "terminal", 0u, NULL, 1u, &argument, out_process_cap);
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
	struct init_configuration configuration;
	struct supervised_process processes[SUPERVISED_PROCESS_COUNT] = {
		[SUPERVISED_VFS]                 = {.description = "VFS"},
		[SUPERVISED_BOOTFS]              = {.description = "bootfs"},
		[SUPERVISED_TERMINAL]            = {.description = "terminal"},
		[SUPERVISED_TTY_SESSION]         = {.description = "TTY session"},
		[SUPERVISED_LOADER]              = {.description = "loader"},
		[SUPERVISED_INPUT_MANAGER]       = {.description = "input manager"},
		[SUPERVISED_KEYBOARD_CONTROLLER] = {.description = "keyboard controller"},
		[SUPERVISED_TEXT_INPUT]          = {.description = "text input"},
		[SUPERVISED_DEVICE_MANAGER]      = {.description = "device manager"},
		[SUPERVISED_SHELL]               = {.description = "shell"},
	};
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
	channel_id_t     supervisor_endpoint = CHANNEL_ID_INVALID;
	cap_id_t         supervisor_activity = CAP_ID_INVALID;
	cap_id_t         supervisor_control  = CAP_ID_INVALID;
	cap_id_t         registry_activity   = CAP_ID_INVALID;
	syscall_status_t status;

	if (init == NULL || init->registry_cap == CAP_ID_INVALID) return 1;
	if (!text_input_layout_override(init, &text_input_layout_override_path)) {
		printf("init: invalid or duplicate %s boot option\n", TEXT_INPUT_LAYOUT_OPTION);
		return 1;
	}
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID || self.self_cap == CAP_ID_INVALID) return 1;
	supervisor_pid = self.pid;
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	status = signal_create(&registry_activity);
	if (status != SYSCALL_STATUS_OK || !watch_service(registry_activity, &vfs_selector) ||
	    !watch_service(registry_activity, &bootfs_selector) || !watch_service(registry_activity, &terminal_selector) ||
	    !watch_service(registry_activity, &tty_session_selector) ||
	    !watch_service(registry_activity, &text_input_selector) || !watch_service(registry_activity, &loader_selector))
		return 1;
	status = channel_create(&supervisor_endpoint, &supervisor_activity);
	if (status == SYSCALL_STATUS_OK)
		status = cap_publish(
			supervisor_endpoint, SUPERVISOR_CONTROL_OBJECT_ID, supervisor_pid, CAP_CALL, &supervisor_control);
	if (status != SYSCALL_STATUS_OK) return 1;
	{
		struct signal_message pending;
		bool                  received;
		if (signal_try_wait(supervisor_activity, &pending, &received) != SYSCALL_STATUS_OK) return 1;
	}
	{
		const struct registry_monitor_argument argument = {
			.registry_activity = registry_activity,
			.control_cap       = supervisor_control,
		};
		if (!spawn_worker(self.self_cap, (uintptr_t)monitor_registry, &argument, sizeof(argument))) return 1;
	}
	for (;;) {
		if (!vfs_started && processes[SUPERVISED_VFS].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_VFS]);
			bool launched = bootstrap_launch(init, "vfs.elf", "VFS", 0u, NULL, 0u, NULL, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_VFS],
			                           SUPERVISED_VFS,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			vfs_started = processes[SUPERVISED_VFS].state == SUPERVISED_PROCESS_RUNNING;
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
		if (vfs_ready && !bootfs_started && processes[SUPERVISED_BOOTFS].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_BOOTFS]);
			bool launched = launch_bootfs(init, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_BOOTFS],
			                           SUPERVISED_BOOTFS,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			bootfs_started = processes[SUPERVISED_BOOTFS].state == SUPERVISED_PROCESS_RUNNING;
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
		if (bootfs_mounted && !terminal_started && processes[SUPERVISED_TERMINAL].state == SUPERVISED_PROCESS_STOPPED) {
			bool     terminal_launched;
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_TERMINAL]);
			bool launched = launch_terminal(init, &terminal_launched, &process_cap);
			if (!launched) {
				if (!process_finish_launch(&processes[SUPERVISED_TERMINAL],
				                           SUPERVISED_TERMINAL,
				                           self.self_cap,
				                           supervisor_control,
				                           false,
				                           process_cap))
					return 1;
				continue;
			}
			if (terminal_launched && !process_finish_launch(&processes[SUPERVISED_TERMINAL],
			                                                SUPERVISED_TERMINAL,
			                                                self.self_cap,
			                                                supervisor_control,
			                                                true,
			                                                process_cap))
				return 1;
			if (terminal_launched && processes[SUPERVISED_TERMINAL].state != SUPERVISED_PROCESS_RUNNING) continue;
			if (!terminal_launched) processes[SUPERVISED_TERMINAL].state = SUPERVISED_PROCESS_DISABLED;
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
				processes[SUPERVISED_TTY_SESSION].state = SUPERVISED_PROCESS_DISABLED;
				tty_session_started                     = true;
				tty_session_ready                       = true;
				continue;
			}
			if (processes[SUPERVISED_TTY_SESSION].state == SUPERVISED_PROCESS_STOPPED) {
				cap_id_t process_cap = CAP_ID_INVALID;
				process_begin_launch(&processes[SUPERVISED_TTY_SESSION]);
				bool launched = bootstrap_launch_terminal_session(
					init, "tty_session.elf", "TTY session", 0u, NULL, 0u, NULL, &process_cap);
				if (!process_finish_launch(&processes[SUPERVISED_TTY_SESSION],
				                           SUPERVISED_TTY_SESSION,
				                           self.self_cap,
				                           supervisor_control,
				                           launched,
				                           process_cap))
					return 1;
				tty_session_started = processes[SUPERVISED_TTY_SESSION].state == SUPERVISED_PROCESS_RUNNING;
				continue;
			}
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
		if (configuration_loaded && !loader_started &&
		    processes[SUPERVISED_LOADER].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_LOADER]);
			bool launched = bootstrap_launch(init, "loader.elf", "loader", 0u, NULL, 0u, NULL, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_LOADER],
			                           SUPERVISED_LOADER,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			loader_started = processes[SUPERVISED_LOADER].state == SUPERVISED_PROCESS_RUNNING;
			continue;
		}
		if (loader_started && !input_manager_started &&
		    processes[SUPERVISED_INPUT_MANAGER].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_INPUT_MANAGER]);
			bool launched =
				bootstrap_launch(init, "input_manager.elf", "input manager", 0u, NULL, 0u, NULL, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_INPUT_MANAGER],
			                           SUPERVISED_INPUT_MANAGER,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			input_manager_started = processes[SUPERVISED_INPUT_MANAGER].state == SUPERVISED_PROCESS_RUNNING;
			continue;
		}
		if (input_manager_started && !keyboard_controller_started &&
		    processes[SUPERVISED_KEYBOARD_CONTROLLER].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_KEYBOARD_CONTROLLER]);
			bool launched = bootstrap_launch(
				init, "keyboard_controller.elf", "keyboard controller", 0u, NULL, 0u, NULL, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_KEYBOARD_CONTROLLER],
			                           SUPERVISED_KEYBOARD_CONTROLLER,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			keyboard_controller_started = processes[SUPERVISED_KEYBOARD_CONTROLLER].state == SUPERVISED_PROCESS_RUNNING;
			continue;
		}
		if (keyboard_controller_started && !text_input_started &&
		    processes[SUPERVISED_TEXT_INPUT].state == SUPERVISED_PROCESS_STOPPED) {
			const char* arguments[] = {text_input_layout};
			cap_id_t    process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_TEXT_INPUT]);
			bool launched =
				bootstrap_launch(init, "text_input.elf", "text input", 1u, arguments, 0u, NULL, &process_cap);
			if (!process_finish_launch(&processes[SUPERVISED_TEXT_INPUT],
			                           SUPERVISED_TEXT_INPUT,
			                           self.self_cap,
			                           supervisor_control,
			                           launched,
			                           process_cap))
				return 1;
			text_input_started = processes[SUPERVISED_TEXT_INPUT].state == SUPERVISED_PROCESS_RUNNING;
			continue;
		}
		if (text_input_started && !device_manager_started &&
		    processes[SUPERVISED_DEVICE_MANAGER].state == SUPERVISED_PROCESS_STOPPED) {
			cap_id_t process_cap = CAP_ID_INVALID;
			process_begin_launch(&processes[SUPERVISED_DEVICE_MANAGER]);
			enum device_manager_launch_result result = device_manager_launch(init, &process_cap);

			if (result == DEVICE_MANAGER_LAUNCH_FAILED) {
				if (!process_schedule_restart(&processes[SUPERVISED_DEVICE_MANAGER],
				                              SUPERVISED_DEVICE_MANAGER,
				                              self.self_cap,
				                              supervisor_control))
					return 1;
				continue;
			}
			if (result == DEVICE_MANAGER_LAUNCH_SUCCESS) {
				if (!process_finish_launch(&processes[SUPERVISED_DEVICE_MANAGER],
				                           SUPERVISED_DEVICE_MANAGER,
				                           self.self_cap,
				                           supervisor_control,
				                           true,
				                           process_cap))
					return 1;
				device_manager_started = processes[SUPERVISED_DEVICE_MANAGER].state == SUPERVISED_PROCESS_RUNNING;
				continue;
			}
			processes[SUPERVISED_DEVICE_MANAGER].state = SUPERVISED_PROCESS_STOPPED;
		}
		if (device_manager_started && !shell_started) {
			if (!terminal_available) {
				processes[SUPERVISED_SHELL].state = SUPERVISED_PROCESS_DISABLED;
				shell_started                     = true;
				continue;
			}
			if (processes[SUPERVISED_SHELL].state == SUPERVISED_PROCESS_STOPPED &&
			    service_available(&text_input_selector) && service_available(&tty_session_selector)) {
				cap_id_t process_cap = CAP_ID_INVALID;
				process_begin_launch(&processes[SUPERVISED_SHELL]);
				bool launched =
					bootstrap_launch_interactive(init, "shell.elf", "shell", 0u, NULL, 0u, NULL, &process_cap);
				if (!process_finish_launch(&processes[SUPERVISED_SHELL],
				                           SUPERVISED_SHELL,
				                           self.self_cap,
				                           supervisor_control,
				                           launched,
				                           process_cap))
					return 1;
				shell_started = processes[SUPERVISED_SHELL].state == SUPERVISED_PROCESS_RUNNING;
				continue;
			}
		}
		bool handled_event = false;
		for (;;) {
			struct cap_request      call;
			struct supervisor_event event;
			bool                    received = false;

			status = channel_recv(supervisor_endpoint, &call, &event, sizeof(event), &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (!received) break;
			handled_event = true;
			if (call.object_id != SUPERVISOR_CONTROL_OBJECT_ID || call.caller != supervisor_pid ||
			    (call.rights & CAP_CALL) == 0u || call.request_size != sizeof(event)) {
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_DENIED) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			if (event.op == SUPERVISOR_EVENT_REGISTRY_CHANGE) {
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_OK) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			if (event.process >= SUPERVISED_PROCESS_COUNT) {
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			struct supervised_process* process = &processes[event.process];
			if (event.generation != process->generation) {
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_OK) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			if (event.op == SUPERVISOR_EVENT_RETRY) {
				if (process->state == SUPERVISED_PROCESS_BACKOFF) process->state = SUPERVISED_PROCESS_STOPPED;
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_OK) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			if (event.op != SUPERVISOR_EVENT_PROCESS_EXIT || process->state != SUPERVISED_PROCESS_RUNNING) {
				if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT) != SYSCALL_STATUS_OK) return 1;
				continue;
			}
			printf("init: %s exited: wait=%u code=%llu\n",
			       process->description,
			       (unsigned)event.wait_status,
			       (unsigned long long)event.exit_code);
			process->process_cap = CAP_ID_INVALID;
			switch (event.process) {
			case SUPERVISED_VFS:
				vfs_started    = false;
				vfs_ready      = false;
				bootfs_mounted = false;
				break;
			case SUPERVISED_BOOTFS:
				bootfs_started = false;
				bootfs_mounted = false;
				break;
			case SUPERVISED_TERMINAL:
				terminal_started   = false;
				terminal_ready     = false;
				terminal_available = false;
				serial_stream_enable();
				display_set_standard_streams(CAP_ID_INVALID, init->serial_stream_cap, init->serial_stream_cap);
				break;
			case SUPERVISED_TTY_SESSION:
				tty_session_started = false;
				tty_session_ready   = false;
				break;
			case SUPERVISED_LOADER:
				loader_started = false;
				break;
			case SUPERVISED_INPUT_MANAGER:
				input_manager_started = false;
				break;
			case SUPERVISED_KEYBOARD_CONTROLLER:
				keyboard_controller_started = false;
				break;
			case SUPERVISED_TEXT_INPUT:
				text_input_started = false;
				break;
			case SUPERVISED_DEVICE_MANAGER:
				device_manager_started = false;
				break;
			case SUPERVISED_SHELL:
				shell_started = false;
				break;
			case SUPERVISED_PROCESS_COUNT:
				return 1;
			}
			if (channel_reply(call.call_id, NULL, 0u, SYSCALL_STATUS_OK) != SYSCALL_STATUS_OK) return 1;
			if (!process_schedule_restart(process, event.process, self.self_cap, supervisor_control)) return 1;
		}
		for (;;) {
			struct channel_event event;
			bool                 received = false;
			status                        = channel_event_recv(supervisor_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (!received) break;
		}
		if (handled_event) continue;
		struct signal_message activity;
		if (signal_wait(supervisor_activity, &activity) != SYSCALL_STATUS_OK) return 1;
	}
}
