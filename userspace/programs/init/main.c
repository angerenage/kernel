#include <base/loader.h>
#include <base/module.h>
#include <base/startup.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/loader.h>
#include <system/module.h>
#include <system/process.h>
#include <system/thread.h>

extern struct init_startup_info g_startup;

static int launch_loader(void) {
	struct module_query_response module;
	struct loader_load_response  loaded;
	struct process_info_response process_info;
	struct process_startup_info  startup;
	cap_id_t                     serial_cap;
	cap_id_t                     thread_cap;
	uintptr_t                    exit_code;
	syscall_status_t             status;

	status = module_resolve("loader.elf", sizeof("loader.elf"), &module);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf module resolution failed: %u\n", (unsigned)status);
		return 1;
	}

	status = loader_load(g_startup.loader_cap, module.cap, &loaded);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader.elf load failed: %u\n", (unsigned)status);
		return 1;
	}

	status = process_get_info(loaded.process_cap, &process_info);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loaded process query failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	status = cap_delegate(g_startup.base.serial_cap, process_info.pid, CAP_WRITE | CAP_CALL, &serial_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: serial capability delegation failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	startup = (struct process_startup_info){
		.size            = sizeof(startup),
		.heap_base       = loaded.heap_base,
		.heap_page_count = loaded.heap_page_count,
		.page_size       = g_startup.base.page_size,
		.serial_cap      = serial_cap,
	};
	status = process_run(loaded.process_cap, loaded.entry, &startup, sizeof(startup), &thread_cap);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process start failed: %u\n", (unsigned)status);
		(void)process_kill(loaded.process_cap, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		return 1;
	}

	printf("init: launched loader.elf pid=%llu process_cap=%llu thread_cap=%llu\n",
	       (unsigned long long)process_info.pid,
	       (unsigned long long)loaded.process_cap,
	       (unsigned long long)thread_cap);

	status = thread_join(thread_cap, &exit_code);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader main thread wait failed: %u\n", (unsigned)status);
		return 1;
	}
	status = process_wait(loaded.process_cap, NULL);
	if (status != SYSCALL_STATUS_OK) {
		printf("init: loader process reap failed: %u\n", (unsigned)status);
		return 1;
	}
	printf("init: loader.elf exited with code %llu\n", (unsigned long long)exit_code);
	return 0;
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	if (g_startup.loader_cap == CAP_ID_INVALID) {
		printf("init: loader capability not available\n");
		return 1;
	}

	return launch_loader();
}
