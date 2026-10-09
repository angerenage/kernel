#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/startup.h>
#include <runtime/heap.h>
#include <stddef.h>
#include <string.h>
#include <system/kernel_resource.h>
#include <system/process.h>

#include "arguments.h"
#include "init.h"

struct init_state g_init = {0};

int main();

extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

__attribute__((noreturn))
void exit(uintptr_t code) {
	process_exit(code);
}

__attribute__((noreturn))
void _start(const struct init_startup_info* startup) {
	struct process_startup_info runtime_startup;
	const char*                 argv_data = NULL;

	memset(__bss_start, 0, __bss_end - __bss_start);

	if (startup == NULL || startup->size < sizeof(*startup) || startup->heap_base == 0u || startup->heap_size == 0u ||
	    startup->memory_allocator_cap == CAP_ID_INVALID || startup->kernel_resources_cap == CAP_ID_INVALID) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	if (startup->argc == 0u) {
		if (startup->argv_offset != 0u || startup->argv_size != 0u || startup->size != sizeof(*startup))
			exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	else {
		if (startup->argv_offset != sizeof(*startup) || startup->argv_size > startup->size - sizeof(*startup) ||
		    startup->size != sizeof(*startup) + startup->argv_size)
			exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
		argv_data = (const char*)startup + startup->argv_offset;
		if (!init_arguments_valid(startup->argc, argv_data, startup->argv_size))
			exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	if (kernel_resource_acquire(startup->kernel_resources_cap, KERNEL_RESOURCE_TYPE_SERIAL, &g_init.serial_cap) !=
	    SYSCALL_STATUS_OK) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	g_init.kernel_resources_cap = startup->kernel_resources_cap;
	g_init.memory_allocator_cap = startup->memory_allocator_cap;
	g_init.argc                 = startup->argc;
	g_init.argv_data            = argv_data;
	g_init.argv_size            = startup->argv_size;
	runtime_startup             = (struct process_startup_info){
		.size                 = sizeof(runtime_startup),
		.heap_base            = startup->heap_base,
		.heap_size            = startup->heap_size,
		.memory_allocator_cap = startup->memory_allocator_cap,
		.init_cap             = CAP_ID_INVALID,
	};
	if (!runtime_heap_init(&runtime_startup)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
	exit((uintptr_t)main());
}
