#include "launcher.h"

#include <base/loader.h>
#include <base/math.h>
#include <base/module.h>
#include <base/startup.h>
#include <base/thread.h>
#include <protocol/filesystem.h>
#include <protocol/terminal.h>
#include <protocol/tty_session.h>
#include <protocol/vfs.h>
#include <runtime/program.h>
#include <runtime/registry.h>
#include <runtime/vfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>
#include <system/signal.h>

#define TERMINAL_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_PROTOCOL_VERSION_MINOR 0u
#define TTY_SESSION_PROTOCOL_VERSION_MAJOR 1u
#define TTY_SESSION_PROTOCOL_VERSION_MINOR 0u
#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u
#define INSTALLED_PROGRAM_DIRECTORY "/bin/"
#define INSTALLED_PROGRAM_LOADER "elf64"

enum bootstrap_input_source {
	BOOTSTRAP_INPUT_NONE,
	BOOTSTRAP_INPUT_TERMINAL,
	BOOTSTRAP_INPUT_TTY_SESSION,
};

static bool drop_owned_capability(cap_id_t* capability, const char* description) {
	syscall_status_t status;

	if (capability == NULL || *capability == CAP_ID_INVALID) return true;
	status = cap_drop(*capability);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s capability drop failed: %u\n", description, (unsigned)status);
		return false;
	}
	*capability = CAP_ID_INVALID;
	return true;
}

static void rollback_bootstrap_process(struct loader_load_response* loaded, cap_id_t* thread_cap,
                                       const char* description) {
	syscall_status_t status;

	if (loaded == NULL || loaded->process_cap == CAP_ID_INVALID) return;
	(void)drop_owned_capability(thread_cap, "bootstrap thread");
	(void)drop_owned_capability(&loaded->address_space_cap, "bootstrap address-space");

	status = process_kill(loaded->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s process kill during rollback failed: %u\n", description, (unsigned)status);
	}
	status = process_wait(loaded->process_cap, NULL);
	if (status == SYSCALL_STATUS_OK) {
		loaded->process_cap       = CAP_ID_INVALID;
		loaded->address_space_cap = CAP_ID_INVALID;
		if (thread_cap != NULL) *thread_cap = CAP_ID_INVALID;
		return;
	}
	printf("init: %s process wait during rollback failed: %u\n", description, (unsigned)status);
	(void)drop_owned_capability(&loaded->process_cap, description);
}

static syscall_status_t delegate_output_stream(const struct init_state* init, process_id_t target, bool delegatable,
                                               cap_id_t* out_capability) {
	const struct registry_protocol_query query = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.minor          = TERMINAL_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle terminal = {.capability = CAP_ID_INVALID};
	struct self_info               self     = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	enum registry_status registry_status;
	syscall_status_t     status;
	cap_rights_t         rights = TERMINAL_OUTPUT_RIGHTS | (delegatable ? CAP_DELEGATE : 0u);

	if (init == NULL || target == PROCESS_PID_INVALID || out_capability == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_capability = CAP_ID_INVALID;
	status          = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (self.pid == PROCESS_PID_INVALID) {
		status = SYSCALL_STATUS_FAILED;
		goto cleanup;
	}
	if (registry_cap_id == CAP_ID_INVALID) {
		status = cap_delegate(init->serial_stream_cap, target, rights, out_capability);
		goto cleanup;
	}
	struct registry_call_result result = registry_acquire(&query, TERMINAL_SERVICE_NAME, &terminal);
	registry_status                    = result.status;
	if (registry_status == REGISTRY_STATUS_OK) {
		status = cap_delegate(terminal.capability, target, rights, out_capability);
		goto cleanup;
	}
	if (registry_status != REGISTRY_STATUS_NOT_FOUND) {
		status = SYSCALL_STATUS_FAILED;
		goto cleanup;
	}
	status = cap_delegate(init->serial_stream_cap, target, rights, out_capability);

cleanup:
	if (terminal.capability != CAP_ID_INVALID && cap_drop(terminal.capability) != SYSCALL_STATUS_OK &&
	    status == SYSCALL_STATUS_OK)
		status = SYSCALL_STATUS_FAILED;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	return status;
}

static syscall_status_t install_input_stream(process_id_t target, enum bootstrap_input_source source, bool delegatable,
                                             cap_id_t* out_capability) {
	struct registry_protocol_query query = {0};
	const char*                    service;
	cap_rights_t                   rights;

	if (target == PROCESS_PID_INVALID || source == BOOTSTRAP_INPUT_NONE || out_capability == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	if (source == BOOTSTRAP_INPUT_TERMINAL) {
		(void)strlcpy(query.namespace_path, TERMINAL_NAMESPACE, sizeof(query.namespace_path));
		(void)strlcpy(query.protocol, TERMINAL_PROTOCOL_NAME, sizeof(query.protocol));
		query.major = TERMINAL_PROTOCOL_VERSION_MAJOR;
		query.minor = TERMINAL_PROTOCOL_VERSION_MINOR;
		service     = TERMINAL_SERVICE_NAME;
		rights      = TERMINAL_TTY_RIGHTS;
	}
	else if (source == BOOTSTRAP_INPUT_TTY_SESSION) {
		(void)strlcpy(query.namespace_path, TTY_SESSION_NAMESPACE, sizeof(query.namespace_path));
		(void)strlcpy(query.protocol, TTY_SESSION_PROTOCOL_NAME, sizeof(query.protocol));
		query.major = TTY_SESSION_PROTOCOL_VERSION_MAJOR;
		query.minor = TTY_SESSION_PROTOCOL_VERSION_MINOR;
		service     = TTY_SESSION_SERVICE_NAME;
		rights      = TTY_SESSION_CONTROL_RIGHTS;
	}
	else return SYSCALL_STATUS_BAD_ARGUMENT;
	if (delegatable) rights |= CAP_DELEGATE;
	*out_capability                    = CAP_ID_INVALID;
	struct registry_call_result result = registry_grant(target, &query, service, rights, out_capability);
	if (result.status == REGISTRY_STATUS_OK) return SYSCALL_STATUS_OK;
	return result.status == REGISTRY_STATUS_NOT_FOUND ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_FAILED;
}

static bool bootstrap_launch_internal(const struct init_state* init, const char* module_name, const char* description,
                                      size_t argc, const char* const argv[], size_t capability_count,
                                      const struct program_capability_argument capabilities[],
                                      enum bootstrap_input_source input_source, bool delegatable_output,
                                      process_id_t* out_pid, cap_id_t* out_process_cap) {
	struct module_provider_resolve_response module = {.cap = CAP_ID_INVALID};
	struct loader_load_response             loaded;
	struct process_info_response            process_info;
	struct process_startup_info*            startup = NULL;
	cap_id_t*                               startup_capabilities;
	size_t                                  startup_capability_count;
	size_t                                  startup_capability_size;
	size_t                                  startup_argv_size = 0u;
	size_t                                  startup_size;
	cap_id_t                                registry_cap      = CAP_ID_INVALID;
	cap_id_t                                loader_cap        = CAP_ID_INVALID;
	cap_id_t                                modules_cap       = CAP_ID_INVALID;
	cap_id_t                                allocator_cap     = CAP_ID_INVALID;
	cap_id_t                                input_stream_cap  = CAP_ID_INVALID;
	cap_id_t                                output_stream_cap = CAP_ID_INVALID;
	cap_id_t                                thread_cap        = CAP_ID_INVALID;
	syscall_status_t                        status;
	bool                                    temporary_caps_released;

	if (init == NULL || module_name == NULL || module_name[0] == '\0' || description == NULL ||
	    (argc != 0u && argv == NULL) || argc > UINT32_MAX || (capability_count != 0u && capabilities == NULL) ||
	    capability_count > UINT32_MAX - PROCESS_STARTUP_CAP_COUNT ||
	    (out_process_cap != NULL && *out_process_cap != CAP_ID_INVALID))
		return false;
	for (size_t i = 0u; i < argc; i++) {
		if (argv[i] == NULL) return false;
		size_t length = strlen(argv[i]);
		if (length == SIZE_MAX || add_overflow_size(startup_argv_size, length + 1u, &startup_argv_size)) return false;
	}
	startup_capability_count = PROCESS_STARTUP_CAP_COUNT + capability_count;
	if (mul_overflow_size(startup_capability_count, sizeof(*startup_capabilities), &startup_capability_size) ||
	    add_overflow_size(sizeof(*startup), startup_capability_size, &startup_size) ||
	    add_overflow_size(startup_size, startup_argv_size, &startup_size) || startup_size > THREAD_START_ARG_MAX_SIZE)
		return false;
	for (size_t i = 0u; i < capability_count; i++) {
		if (capabilities[i].capability != CAP_ID_INVALID && capabilities[i].rights == 0u) return false;
	}
	loaded.process_cap       = CAP_ID_INVALID;
	loaded.address_space_cap = CAP_ID_INVALID;
	status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_LOADER, &loader_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: kernel loader acquisition failed: %u\n", (unsigned)status);
		return false;
	}
	status = kernel_resource_acquire(init->kernel_resources_cap, KERNEL_RESOURCE_TYPE_MODULES, &modules_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: modules provider acquisition failed: %u\n", (unsigned)status);
		(void)drop_owned_capability(&loader_cap, "kernel loader");
		return false;
	}
	status = module_resolve(modules_cap, module_name, strlen(module_name) + 1u, &module);
	if (!drop_owned_capability(&modules_cap, "modules provider")) {
		(void)drop_owned_capability(&module.cap, "bootstrap module");
		(void)drop_owned_capability(&loader_cap, "kernel loader");
		return false;
	}
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s module resolution failed: %u\n", module_name, (unsigned)status);
		(void)drop_owned_capability(&loader_cap, "kernel loader");
		return false;
	}
	status = loader_load(loader_cap, module.cap, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s load failed: %u\n", module_name, (unsigned)status);
	}
	temporary_caps_released = drop_owned_capability(&module.cap, "bootstrap module");
	if (!drop_owned_capability(&loader_cap, "kernel loader")) temporary_caps_released = false;
	if (status != SYSCALL_STATUS_OK) {
		rollback_bootstrap_process(&loaded, &thread_cap, description);
		return false;
	}
	if (!temporary_caps_released) goto fail;
	if (!drop_owned_capability(&loaded.address_space_cap, "bootstrap address-space")) goto fail;
	status = process_get_info(loaded.process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s process query failed: %u\n", description, (unsigned)status);
		goto fail;
	}
	status = cap_delegate(init->memory_allocator_cap,
	                      process_info.pid,
	                      CAP_CALL | CAP_READ | CAP_ALLOCATE | CAP_DELEGATE,
	                      &allocator_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: MemoryAllocator delegation failed: %u\n", (unsigned)status);
		goto fail;
	}
	if (init->registry_cap != CAP_ID_INVALID) {
		status = cap_delegate_peer(
			init->registry_cap, process_info.pid, CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER, &registry_cap);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: registry capability delegation failed: %u\n", (unsigned)status);
			goto fail;
		}
	}
	status = delegate_output_stream(init, process_info.pid, delegatable_output, &output_stream_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: output stream delegation failed: %u\n", (unsigned)status);
		goto fail;
	}
	if (input_source != BOOTSTRAP_INPUT_NONE) {
		status = install_input_stream(process_info.pid, input_source, false, &input_stream_cap);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: input stream installation failed: %u\n", (unsigned)status);
			goto fail;
		}
	}
	startup = calloc(1u, startup_size);
	if (startup == NULL) goto fail;
	*startup = (struct process_startup_info){
		.size                 = (uint32_t)startup_size,
		.heap_base            = loaded.heap_base,
		.heap_size            = loaded.heap_size,
		.memory_allocator_cap = allocator_cap,
		.registry_cap         = registry_cap,
		.capc                 = (uint32_t)startup_capability_count,
		.capv_offset          = sizeof(*startup),
		.argc                 = (uint32_t)argc,
		.argv_offset          = argc == 0u ? 0u : (uint32_t)(sizeof(*startup) + startup_capability_size),
		.argv_size            = (uint32_t)startup_argv_size,
	};
	startup_capabilities                             = (cap_id_t*)((uint8_t*)startup + startup->capv_offset);
	startup_capabilities[PROCESS_STARTUP_CAP_STDIN]  = input_stream_cap;
	startup_capabilities[PROCESS_STARTUP_CAP_STDOUT] = output_stream_cap;
	startup_capabilities[PROCESS_STARTUP_CAP_STDERR] = output_stream_cap;
	for (size_t i = 0u; i < capability_count; i++) {
		if (capabilities[i].capability == CAP_ID_INVALID) continue;
		status = cap_delegate(capabilities[i].capability,
		                      process_info.pid,
		                      capabilities[i].rights,
		                      &startup_capabilities[PROCESS_STARTUP_CAP_COUNT + i]);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: %s capability delegation failed: %u\n", description, (unsigned)status);
			goto fail;
		}
	}
	char* argument_cursor = (char*)startup + startup->argv_offset;
	for (size_t i = 0u; i < argc; i++) {
		size_t length = strlen(argv[i]) + 1u;
		memcpy(argument_cursor, argv[i], length);
		argument_cursor += length;
	}
	status = process_run(loaded.process_cap, loaded.entry, startup, startup_size, &thread_cap);
	free(startup);
	startup = NULL;
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s process start failed: %u\n", description, (unsigned)status);
		goto fail;
	}
	if (!drop_owned_capability(&thread_cap, "bootstrap thread")) goto fail;
	if (out_pid != NULL) *out_pid = process_info.pid;
	if (out_process_cap != NULL) {
		*out_process_cap   = loaded.process_cap;
		loaded.process_cap = CAP_ID_INVALID;
	}
	else {
		status = process_detach(loaded.process_cap);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: %s process detach failed: %u\n", description, (unsigned)status);
			goto fail;
		}
		(void)drop_owned_capability(&loaded.process_cap, "bootstrap process");
	}
	return true;

fail:
	free(startup);
	rollback_bootstrap_process(&loaded, &thread_cap, description);
	return false;
}

bool bootstrap_launch_registry(struct init_state* init) {
	char                               init_pid_argument[32];
	const char*                        arguments[] = {init_pid_argument};
	struct program_capability_argument capability;
	struct signal_message              message;
	struct self_info                   self;
	process_id_t                       registry_pid = PROCESS_PID_INVALID;
	cap_id_t                           ready_cap    = CAP_ID_INVALID;
	syscall_status_t                   status;
	bool                               launched;

	if (init == NULL || init->registry_cap != CAP_ID_INVALID) return false;
	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	if (snprintf(init_pid_argument, sizeof(init_pid_argument), "%llu", (unsigned long long)self.pid) <= 0) return false;
	status = signal_create(&ready_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	capability = (struct program_capability_argument){.capability = ready_cap, .rights = CAP_SIGNAL};
	launched   = bootstrap_launch_internal(init,
	                                       "registry.elf",
	                                       "registry",
	                                       1u,
	                                       arguments,
	                                       1u,
	                                       &capability,
	                                       BOOTSTRAP_INPUT_NONE,
	                                       false,
	                                       &registry_pid,
	                                       NULL);
	if (!launched) {
		(void)signal_destroy(ready_cap);
		return false;
	}
	status = signal_wait(ready_cap, &message);
	(void)signal_destroy(ready_cap);
	if (status != SYSCALL_STATUS_OK || message.sender != registry_pid || message.payload.args[0] == CAP_ID_INVALID)
		return false;
	init->registry_cap = message.payload.args[0];
	registry_cap_id    = init->registry_cap;
	return true;
}

bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description, size_t argc,
                      const char* const argv[], size_t capability_count,
                      const struct program_capability_argument capabilities[], cap_id_t* out_process_cap) {
	return bootstrap_launch_internal(init,
	                                 module_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_NONE,
	                                 false,
	                                 NULL,
	                                 out_process_cap);
}

bool bootstrap_launch_terminal_session(const struct init_state* init, const char* module_name, const char* description,
                                       size_t argc, const char* const argv[], size_t capability_count,
                                       const struct program_capability_argument capabilities[],
                                       cap_id_t*                                out_process_cap) {
	return bootstrap_launch_internal(init,
	                                 module_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_TERMINAL,
	                                 false,
	                                 NULL,
	                                 out_process_cap);
}

bool bootstrap_launch_interactive(const struct init_state* init, const char* module_name, const char* description,
                                  size_t argc, const char* const argv[], size_t capability_count,
                                  const struct program_capability_argument capabilities[], cap_id_t* out_process_cap) {
	return bootstrap_launch_internal(init,
	                                 module_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_TTY_SESSION,
	                                 true,
	                                 NULL,
	                                 out_process_cap);
}

static syscall_status_t acquire_output_stream(const struct init_state* init, cap_id_t* out_capability,
                                              bool* out_owned) {
	const struct registry_protocol_query query = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.minor          = TERMINAL_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle terminal = {.capability = CAP_ID_INVALID};
	struct registry_call_result    result;

	if (init == NULL || out_capability == NULL || out_owned == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_capability = CAP_ID_INVALID;
	*out_owned      = false;
	result          = registry_acquire(&query, TERMINAL_SERVICE_NAME, &terminal);
	if (result.status == REGISTRY_STATUS_OK && result.transport_status == SYSCALL_STATUS_OK) {
		*out_capability = terminal.capability;
		*out_owned      = true;
		return SYSCALL_STATUS_OK;
	}
	if (result.status != REGISTRY_STATUS_NOT_FOUND) return SYSCALL_STATUS_FAILED;
	*out_capability = init->serial_stream_cap;
	return SYSCALL_STATUS_OK;
}

static bool installed_launch_internal(const struct init_state* init, const char* file_name, const char* description,
                                      size_t argc, const char* const argv[], size_t capability_count,
                                      const struct program_capability_argument capabilities[],
                                      enum bootstrap_input_source input_source, bool delegatable_output,
                                      cap_id_t* out_process_cap) {
	const struct registry_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle      vfs        = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle       executable = {.capability = CAP_ID_INVALID};
	struct program_load_result          loaded     = {.load_cap = CAP_ID_INVALID, .process_id = PROCESS_PID_INVALID};
	struct program_run_result           running    = {.process_cap = CAP_ID_INVALID, .thread_cap = CAP_ID_INVALID};
	struct program_capability_argument* capv       = NULL;
	cap_id_t                            input_cap  = CAP_ID_INVALID;
	cap_id_t                            output_cap = CAP_ID_INVALID;
	char*                               path       = NULL;
	size_t                              file_name_size;
	size_t                              path_size;
	syscall_status_t                    status;
	bool                                output_owned = false;
	bool                                success      = false;
	process_id_t                        init_pid     = PROCESS_PID_INVALID;

	if (init == NULL || file_name == NULL || file_name[0] == '\0' || strchr(file_name, '/') != NULL ||
	    description == NULL || (argc != 0u && argv == NULL) || (capability_count != 0u && capabilities == NULL) ||
	    capability_count > SIZE_MAX - PROCESS_STARTUP_CAP_COUNT || out_process_cap == NULL ||
	    *out_process_cap != CAP_ID_INVALID)
		return false;
	file_name_size = strlen(file_name);
	if (file_name_size > SIZE_MAX - sizeof(INSTALLED_PROGRAM_DIRECTORY)) return false;
	path_size = sizeof(INSTALLED_PROGRAM_DIRECTORY) - 1u + file_name_size;
	path      = malloc(path_size + 1u);
	if (path == NULL) goto cleanup;
	memcpy(path, INSTALLED_PROGRAM_DIRECTORY, sizeof(INSTALLED_PROGRAM_DIRECTORY) - 1u);
	memcpy(path + sizeof(INSTALLED_PROGRAM_DIRECTORY) - 1u, file_name, file_name_size + 1u);
	struct registry_call_result registry_result = registry_acquire(&vfs_query, VFS_SERVICE_NAME, &vfs);
	if (registry_result.status != REGISTRY_STATUS_OK || registry_result.transport_status != SYSCALL_STATUS_OK) {
		printf("init: VFS acquisition for %s failed: %u\n", description, (unsigned)registry_result.status);
		goto cleanup;
	}
	struct filesystem_call_result open_result =
		vfs_open(vfs.capability, path, path_size, CAP_READ | CAP_DELEGATE, &executable);
	if (open_result.transport_status != SYSCALL_STATUS_OK || open_result.status != FILESYSTEM_STATUS_OK ||
	    executable.info.type != FILESYSTEM_NODE_FILE) {
		printf("init: opening %s failed: transport=%u status=%u\n",
		       path,
		       (unsigned)open_result.transport_status,
		       (unsigned)open_result.status);
		goto cleanup;
	}
	status = program_load(INSTALLED_PROGRAM_LOADER, executable.capability, file_name, file_name_size, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loading %s failed: %u\n", path, (unsigned)status);
		goto cleanup;
	}
	if (input_source != BOOTSTRAP_INPUT_NONE) {
		struct self_info self = {
			.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
		status = process_self_info(&self);
		if (status == SYSCALL_STATUS_OK) init_pid = self.pid;
		if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
		if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
		if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
		if (status != SYSCALL_STATUS_OK || init_pid == PROCESS_PID_INVALID) goto cleanup;
		status = install_input_stream(init_pid, input_source, true, &input_cap);
		if (status != SYSCALL_STATUS_OK) {
			printf("init: input stream acquisition for %s failed: %u\n", description, (unsigned)status);
			goto cleanup;
		}
	}
	status = acquire_output_stream(init, &output_cap, &output_owned);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: output stream acquisition for %s failed: %u\n", description, (unsigned)status);
		goto cleanup;
	}
	capv = calloc(PROCESS_STARTUP_CAP_COUNT + capability_count, sizeof(*capv));
	if (capv == NULL) goto cleanup;
	capv[PROCESS_STARTUP_CAP_STDIN] = (struct program_capability_argument){
		.capability = input_cap,
		.rights = input_cap == CAP_ID_INVALID
		              ? 0u
		              : (input_source == BOOTSTRAP_INPUT_TERMINAL ? TERMINAL_TTY_RIGHTS : TTY_SESSION_CONTROL_RIGHTS),
	};
	capv[PROCESS_STARTUP_CAP_STDOUT] = (struct program_capability_argument){
		.capability = output_cap,
		.rights     = TERMINAL_OUTPUT_RIGHTS | (delegatable_output ? CAP_DELEGATE : 0u),
	};
	capv[PROCESS_STARTUP_CAP_STDERR] = capv[PROCESS_STARTUP_CAP_STDOUT];
	for (size_t index = 0u; index < capability_count; index++)
		capv[PROCESS_STARTUP_CAP_COUNT + index] = capabilities[index];
	status = program_run(&loaded, argc, argv, PROCESS_STARTUP_CAP_COUNT + capability_count, capv, &running);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: starting %s failed: %u\n", path, (unsigned)status);
		goto cleanup;
	}
	loaded.load_cap = CAP_ID_INVALID;
	if (!drop_owned_capability(&running.thread_cap, "installed program thread")) goto cleanup;
	*out_process_cap    = running.process_cap;
	running.process_cap = CAP_ID_INVALID;
	success             = true;

cleanup:
	free(capv);
	free(path);
	if (output_owned) (void)drop_owned_capability(&output_cap, "installed program output");
	if (input_cap != CAP_ID_INVALID) (void)drop_owned_capability(&input_cap, "installed program input");
	if (executable.capability != CAP_ID_INVALID)
		(void)drop_owned_capability(&executable.capability, "installed executable");
	if (vfs.capability != CAP_ID_INVALID) (void)drop_owned_capability(&vfs.capability, "VFS");
	if (loaded.load_cap != CAP_ID_INVALID) (void)program_cancel(loaded.load_cap);
	if (!success && running.process_cap != CAP_ID_INVALID) {
		(void)process_kill(running.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		(void)process_wait(running.process_cap, NULL);
	}
	return success;
}

bool installed_launch(const struct init_state* init, const char* file_name, const char* description, size_t argc,
                      const char* const argv[], size_t capability_count,
                      const struct program_capability_argument capabilities[], cap_id_t* out_process_cap) {
	return installed_launch_internal(init,
	                                 file_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_NONE,
	                                 false,
	                                 out_process_cap);
}

bool installed_launch_terminal_session(const struct init_state* init, const char* file_name, const char* description,
                                       size_t argc, const char* const argv[], size_t capability_count,
                                       const struct program_capability_argument capabilities[],
                                       cap_id_t*                                out_process_cap) {
	return installed_launch_internal(init,
	                                 file_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_TERMINAL,
	                                 false,
	                                 out_process_cap);
}

bool installed_launch_interactive(const struct init_state* init, const char* file_name, const char* description,
                                  size_t argc, const char* const argv[], size_t capability_count,
                                  const struct program_capability_argument capabilities[], cap_id_t* out_process_cap) {
	return installed_launch_internal(init,
	                                 file_name,
	                                 description,
	                                 argc,
	                                 argv,
	                                 capability_count,
	                                 capabilities,
	                                 BOOTSTRAP_INPUT_TTY_SESSION,
	                                 true,
	                                 out_process_cap);
}
