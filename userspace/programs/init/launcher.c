#include "launcher.h"

#include <base/loader.h>
#include <base/module.h>
#include <base/startup.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>

#include "server.h"

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

bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description) {
	struct module_provider_resolve_response module = {.cap = CAP_ID_INVALID};
	struct loader_load_response             loaded;
	struct process_info_response            process_info;
	struct {
		struct process_startup_info info;
		cap_id_t                    capabilities[PROCESS_STARTUP_CAP_COUNT];
	} startup;
	cap_id_t         init_cap          = CAP_ID_INVALID;
	cap_id_t         loader_cap        = CAP_ID_INVALID;
	cap_id_t         modules_cap       = CAP_ID_INVALID;
	cap_id_t         allocator_cap     = CAP_ID_INVALID;
	cap_id_t         serial_stream_cap = CAP_ID_INVALID;
	cap_id_t         thread_cap        = CAP_ID_INVALID;
	syscall_status_t status;
	bool             temporary_caps_released;

	if (init == NULL || module_name == NULL || module_name[0] == '\0' || description == NULL) return false;
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
	status = cap_delegate(init->serial_stream_cap, process_info.pid, CAP_WRITE | CAP_CALL, &serial_stream_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: serial stream delegation failed: %u\n", (unsigned)status);
		goto fail;
	}
	startup = (typeof(startup)){
		.info =
			{
				   .size                 = sizeof(startup),
				   .heap_base            = loaded.heap_base,
				   .heap_size            = loaded.heap_size,
				   .memory_allocator_cap = allocator_cap,
				   .init_cap             = init_cap,
				   .capc                 = PROCESS_STARTUP_CAP_COUNT,
				   .capv_offset          = sizeof(startup.info),
				   },
		.capabilities =
			{
				   [PROCESS_STARTUP_CAP_STDIN]  = CAP_ID_INVALID,
				   [PROCESS_STARTUP_CAP_STDOUT] = serial_stream_cap,
				   [PROCESS_STARTUP_CAP_STDERR] = serial_stream_cap,
				   },
	};
	status = process_run(loaded.process_cap, loaded.entry, &startup, sizeof(startup), &thread_cap);
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
	rollback_bootstrap_process(&loaded, &thread_cap, description);
	return false;
}
