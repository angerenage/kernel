#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/startup.h>
#include <runtime/heap.h>
#include <string.h>
#include <system/display.h>
#include <system/kernel_resource.h>
#include <system/process.h>

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

	memset(__bss_start, 0, __bss_end - __bss_start);

	if (startup == NULL || startup->size < sizeof(*startup) || startup->heap_base == 0u ||
	    startup->heap_page_count == 0u || startup->page_size == 0u || startup->kernel_resources_cap == CAP_ID_INVALID) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	if (kernel_resource_acquire(startup->kernel_resources_cap, KERNEL_RESOURCE_TYPE_SERIAL, &g_init.serial_cap) !=
	    SYSCALL_STATUS_OK) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	g_init.kernel_resources_cap = startup->kernel_resources_cap;
	g_init.page_size            = startup->page_size;
	serial_cap_id               = g_init.serial_cap;
	runtime_startup             = (struct process_startup_info){
					.size            = sizeof(runtime_startup),
					.heap_base       = startup->heap_base,
					.heap_page_count = startup->heap_page_count,
					.page_size       = startup->page_size,
					.serial_cap      = g_init.serial_cap,
					.init_cap        = CAP_ID_INVALID,
    };
	if (!runtime_heap_init(&runtime_startup)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
	exit((uintptr_t)main());
}
