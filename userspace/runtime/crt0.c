#include <base/cap.h>
#include <base/startup.h>
#include <runtime/heap.h>
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
	memset(__bss_start, 0, __bss_end - __bss_start);
	if (startup == NULL || startup->size < sizeof(*startup) || startup->heap_base == 0u ||
	    startup->heap_page_count == 0u || startup->page_size == 0u) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	serial_cap_id = startup->serial_cap;
	if (!runtime_heap_init(startup)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
	exit((uintptr_t)main(0, (char**)0));
}
