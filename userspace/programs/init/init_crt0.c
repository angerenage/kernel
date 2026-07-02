#include <base/cap.h>
#include <base/startup.h>
#include <runtime/heap.h>
#include <string.h>
#include <system/display.h>
#include <system/process.h>

struct init_startup_info g_startup = {0};

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

	if (startup == NULL || startup->size < sizeof(g_startup)) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}

	memcpy(&g_startup, startup, sizeof(g_startup));

	if (g_startup.base.heap_base == 0u || g_startup.base.heap_page_count == 0u || g_startup.base.page_size == 0u) {
		exit(PROCESS_EXIT_SYSTEM_INVALID_STARTUP);
	}
	serial_cap_id = g_startup.base.serial_cap;
	if (!runtime_heap_init(&g_startup.base)) {
		exit(PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
	}
	exit((uintptr_t)main(0, (char**)0));
}
