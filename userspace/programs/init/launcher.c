#include "launcher.h"

#include <base/loader.h>
#include <base/module.h>
#include <base/startup.h>
#include <stdio.h>
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

static void rollback_loader_process(struct loader_load_response* loaded, cap_id_t* thread_cap) {
	syscall_status_t status;

	if (loaded == NULL || loaded->process_cap == CAP_ID_INVALID) return;
	(void)drop_owned_capability(thread_cap, "loader thread");
	(void)drop_owned_capability(&loaded->address_space_cap, "loader address-space");

	status = process_kill(loaded->process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process kill during rollback failed: %u\n", (unsigned)status);
	}
	status = process_wait(loaded->process_cap, NULL);
	if (status == SYSCALL_STATUS_OK) {
		loaded->process_cap       = CAP_ID_INVALID;
		loaded->address_space_cap = CAP_ID_INVALID;
		if (thread_cap != NULL) *thread_cap = CAP_ID_INVALID;
		return;
	}
	printf("init: loader process wait during rollback failed: %u\n", (unsigned)status);
	(void)drop_owned_capability(&loaded->process_cap, "loader process");
}

bool loader_launch(const struct init_state* init) {
	struct module_provider_resolve_response module = {.cap = CAP_ID_INVALID};
	struct loader_load_response             loaded;
	struct process_info_response            process_info;
	struct process_startup_info             startup;
	cap_id_t                                init_cap    = CAP_ID_INVALID;
	cap_id_t                                loader_cap  = CAP_ID_INVALID;
	cap_id_t                                modules_cap = CAP_ID_INVALID;
	cap_id_t                                serial_cap  = CAP_ID_INVALID;
	cap_id_t                                thread_cap  = CAP_ID_INVALID;
	syscall_status_t                        status;
	bool                                    temporary_caps_released;

	if (init == NULL) return false;
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
	status = module_resolve(modules_cap, "loader.elf", sizeof("loader.elf"), &module);
	if (!drop_owned_capability(&modules_cap, "modules provider")) {
		(void)drop_owned_capability(&module.cap, "loader module");
		(void)drop_owned_capability(&loader_cap, "kernel loader");
		return false;
	}
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf module resolution failed: %u\n", (unsigned)status);
		(void)drop_owned_capability(&loader_cap, "kernel loader");
		return false;
	}
	status = loader_load(loader_cap, module.cap, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf load failed: %u\n", (unsigned)status);
	}
	temporary_caps_released = drop_owned_capability(&module.cap, "loader module");
	if (!drop_owned_capability(&loader_cap, "kernel loader")) temporary_caps_released = false;
	if (status != SYSCALL_STATUS_OK) {
		rollback_loader_process(&loaded, &thread_cap);
		return false;
	}
	if (!temporary_caps_released) goto fail;
	if (!drop_owned_capability(&loaded.address_space_cap, "loader address-space")) goto fail;

	status = process_get_info(loaded.process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loaded process query failed: %u\n", (unsigned)status);
		goto fail;
	}
	status = init_server_grant(process_info.pid, &init_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: init capability publication failed: %u\n", (unsigned)status);
		goto fail;
	}
	status = cap_delegate(
		init->serial_cap, process_info.pid, CAP_WRITE | CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER, &serial_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: serial capability delegation failed: %u\n", (unsigned)status);
		goto fail;
	}
	startup = (struct process_startup_info){
		.size            = sizeof(startup),
		.heap_base       = loaded.heap_base,
		.heap_page_count = loaded.heap_page_count,
		.page_size       = init->page_size,
		.serial_cap      = serial_cap,
		.init_cap        = init_cap,
	};
	status = process_run(loaded.process_cap, loaded.entry, &startup, sizeof(startup), &thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process start failed: %u\n", (unsigned)status);
		goto fail;
	}
	if (!drop_owned_capability(&thread_cap, "loader thread")) goto fail;
	status = process_detach(loaded.process_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process detach failed: %u\n", (unsigned)status);
		goto fail;
	}

	(void)drop_owned_capability(&loaded.process_cap, "loader process");
	return true;

fail:
	rollback_loader_process(&loaded, &thread_cap);
	return false;
}
