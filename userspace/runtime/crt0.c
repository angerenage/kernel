#include <base/cap.h>
#include <base/startup.h>
#include <runtime/heap.h>
#include <runtime/init.h>
#include <runtime/startup.h>
#include <string.h>
#include <system/display.h>
#include <system/process.h>

int main(int argc, char** argv);

extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

__attribute__((noreturn))
void exit(uintptr_t code) {
	process_exit(code);
}

__attribute__((noreturn))
void _start(const struct process_startup_info* startup) {
	enum runtime_startup_argv_result argv_result;
	char**                           argv = NULL;
	int                              argc = 0;
	int                              main_result;

	memset(__bss_start, 0, __bss_end - __bss_start);
	if (startup == NULL || startup->size < sizeof(*startup) || startup->heap_base == 0u ||
	    startup->heap_page_count == 0u || startup->page_size == 0u) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	serial_cap_id = startup->serial_cap;
	init_cap_id   = startup->init_cap;
	if (!runtime_heap_init(startup)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}

	argv_result = runtime_startup_unpack_argv(startup, &argc, &argv);
	if (argv_result == RUNTIME_STARTUP_ARGV_INVALID) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	if (argv_result == RUNTIME_STARTUP_ARGV_NO_MEMORY) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}

	main_result = main(argc, argv);
	runtime_startup_free_argv(argv);
	exit((uintptr_t)main_result);
}
