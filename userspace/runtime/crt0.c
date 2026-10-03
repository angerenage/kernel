#include <base/cap.h>
#include <base/startup.h>
#include <runtime/heap.h>
#include <runtime/init.h>
#include <runtime/startup.h>
#include <string.h>
#include <system/display.h>
#include <system/process.h>

int main(int argc, char** argv, size_t capc, const cap_id_t* capv);

extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

__attribute__((noreturn))
void exit(uintptr_t code) {
	process_exit(code);
}

__attribute__((noreturn))
void _start(const struct process_startup_info* startup) {
	struct runtime_startup_arguments arguments;
	enum runtime_startup_result      startup_result;
	int                              main_result;

	memset(__bss_start, 0, __bss_end - __bss_start);
	if (startup == NULL || startup->size < sizeof(*startup) || startup->heap_base == 0u || startup->heap_size == 0u ||
	    startup->memory_allocator_cap == CAP_ID_INVALID) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	serial_cap_id = startup->serial_cap;
	init_cap_id   = startup->init_cap;
	if (!runtime_heap_init(startup)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}

	startup_result = runtime_startup_unpack(startup, &arguments);
	if (startup_result == RUNTIME_STARTUP_INVALID) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	if (startup_result == RUNTIME_STARTUP_NO_MEMORY) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}

	main_result = main(arguments.argc, arguments.argv, arguments.capc, arguments.capv);
	runtime_startup_arguments_deinit(&arguments);
	exit((uintptr_t)main_result);
}
