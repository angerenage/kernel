#include "launcher.h"

#include <base/loader.h>
#include <base/math.h>
#include <base/module.h>
#include <base/startup.h>
#include <base/thread.h>
#include <protocol/terminal.h>
#include <protocol/tty_session.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>

#include "registry.h"
#include "server.h"

#define TERMINAL_PROTOCOL_VERSION_MAJOR 1u
#define TERMINAL_PROTOCOL_VERSION_MINOR 0u
#define TTY_SESSION_PROTOCOL_VERSION_MAJOR 1u
#define TTY_SESSION_PROTOCOL_VERSION_MINOR 0u

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
	const struct init_protocol_query query = {
		.namespace_path = TERMINAL_NAMESPACE,
		.protocol       = TERMINAL_PROTOCOL_NAME,
		.major          = TERMINAL_PROTOCOL_VERSION_MAJOR,
		.minor          = TERMINAL_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle terminal = {.capability = CAP_ID_INVALID};
	struct self_info           self     = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	enum init_registry_status registry_status;
	syscall_status_t          status;
	cap_rights_t              rights = TERMINAL_OUTPUT_RIGHTS | (delegatable ? CAP_DELEGATE : 0u);

	if (init == NULL || target == PROCESS_PID_INVALID || out_capability == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_capability = CAP_ID_INVALID;
	status          = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK) goto cleanup;
	if (self.pid == PROCESS_PID_INVALID) {
		status = SYSCALL_STATUS_FAILED;
		goto cleanup;
	}
	registry_status = registry_acquire(self.pid, &query, TERMINAL_SERVICE_NAME, &terminal);
	if (registry_status == INIT_REGISTRY_OK) {
		status = cap_delegate(terminal.capability, target, rights, out_capability);
		goto cleanup;
	}
	if (registry_status != INIT_REGISTRY_NOT_FOUND) {
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

static syscall_status_t install_input_stream(process_id_t target, enum bootstrap_input_source source,
                                             cap_id_t* out_capability) {
	struct init_protocol_query query = {0};
	enum init_registry_status  registry_status;
	const char*                service;
	cap_rights_t               rights;

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
	*out_capability = CAP_ID_INVALID;
	registry_status = registry_grant(target, &query, service, rights, out_capability);
	if (registry_status == INIT_REGISTRY_OK) return SYSCALL_STATUS_OK;
	return registry_status == INIT_REGISTRY_NOT_FOUND ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_FAILED;
}

static bool bootstrap_launch_internal(const struct init_state* init, const char* module_name, const char* description,
                                      size_t argc, const char* const argv[], size_t capability_count,
                                      const struct program_capability_argument capabilities[],
                                      enum bootstrap_input_source input_source, bool delegatable_output) {
	struct module_provider_resolve_response module = {.cap = CAP_ID_INVALID};
	struct loader_load_response             loaded;
	struct process_info_response            process_info;
	struct process_startup_info*            startup = NULL;
	cap_id_t*                               startup_capabilities;
	size_t                                  startup_capability_count;
	size_t                                  startup_capability_size;
	size_t                                  startup_argv_size = 0u;
	size_t                                  startup_size;
	cap_id_t                                init_cap          = CAP_ID_INVALID;
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
	    capability_count > UINT32_MAX - PROCESS_STARTUP_CAP_COUNT)
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
	status = init_server_grant(process_info.pid, &init_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: init capability publication failed: %u\n", (unsigned)status);
		goto fail;
	}
	status = delegate_output_stream(init, process_info.pid, delegatable_output, &output_stream_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: output stream delegation failed: %u\n", (unsigned)status);
		goto fail;
	}
	if (input_source != BOOTSTRAP_INPUT_NONE) {
		status = install_input_stream(process_info.pid, input_source, &input_stream_cap);
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
		.init_cap             = init_cap,
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
	status = process_detach(loaded.process_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: %s process detach failed: %u\n", description, (unsigned)status);
		goto fail;
	}

	(void)drop_owned_capability(&loaded.process_cap, "bootstrap process");
	return true;

fail:
	free(startup);
	rollback_bootstrap_process(&loaded, &thread_cap, description);
	return false;
}

bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description, size_t argc,
                      const char* const argv[], size_t capability_count,
                      const struct program_capability_argument capabilities[]) {
	return bootstrap_launch_internal(
		init, module_name, description, argc, argv, capability_count, capabilities, BOOTSTRAP_INPUT_NONE, false);
}

bool bootstrap_launch_terminal_session(const struct init_state* init, const char* module_name, const char* description,
                                       size_t argc, const char* const argv[], size_t capability_count,
                                       const struct program_capability_argument capabilities[]) {
	return bootstrap_launch_internal(
		init, module_name, description, argc, argv, capability_count, capabilities, BOOTSTRAP_INPUT_TERMINAL, false);
}

bool bootstrap_launch_interactive(const struct init_state* init, const char* module_name, const char* description,
                                  size_t argc, const char* const argv[], size_t capability_count,
                                  const struct program_capability_argument capabilities[]) {
	return bootstrap_launch_internal(
		init, module_name, description, argc, argv, capability_count, capabilities, BOOTSTRAP_INPUT_TTY_SESSION, true);
}
