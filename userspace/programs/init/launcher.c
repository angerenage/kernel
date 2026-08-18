#include "launcher.h"

#include <base/loader.h>
#include <base/module.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>

#include "server.h"

bool loader_launch(const struct init_startup_info* init_startup) {
	struct module_query_response module;
	struct loader_load_response  loaded;
	struct process_info_response process_info;
	struct process_startup_info  startup;
	cap_id_t                     init_cap;
	cap_id_t                     serial_cap;
	cap_id_t                     thread_cap;
	syscall_status_t             status;

	if (init_startup == NULL) return false;
	status = module_resolve("loader.elf", sizeof("loader.elf"), &module);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf module resolution failed: %u\n", (unsigned)status);
		return false;
	}
	status = loader_load(init_startup->loader_cap, module.cap, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf load failed: %u\n", (unsigned)status);
		return false;
	}
	status = process_get_info(loaded.process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loaded process query failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return false;
	}
	status = init_server_grant(process_info.pid, &init_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: init capability publication failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return false;
	}
	status =
		cap_delegate(init_startup->base.serial_cap, process_info.pid, CAP_WRITE | CAP_CALL | CAP_DELEGATE, &serial_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: serial capability delegation failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return false;
	}
	startup = (struct process_startup_info){
		.size            = sizeof(startup),
		.heap_base       = loaded.heap_base,
		.heap_page_count = loaded.heap_page_count,
		.page_size       = init_startup->base.page_size,
		.serial_cap      = serial_cap,
		.init_cap        = init_cap,
	};
	status = process_run(loaded.process_cap, loaded.entry, &startup, sizeof(startup), &thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process start failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return false;
	}
	status = process_detach(loaded.process_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process detach failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return false;
	}

	return true;
}
