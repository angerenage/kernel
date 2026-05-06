#include <runtime.h>
#include <string.h>
#include <syscall.h>

extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

__attribute__((noreturn))
void exit(uintptr_t code) {
	(void)exit_process(code);
	for (;;) {
		(void)exit_thread(code);
	}
}

__attribute__((noreturn))
void _start(uintptr_t arg) {
	(void)arg;

	memset(__bss_start, 0, __bss_end - __bss_start);
	exit((uintptr_t)main(0, (char**)0));
}
