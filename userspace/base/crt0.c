#include <base/cap.h>
#include <runtime.h>
#include <string.h>
#include <syscall.h>

extern unsigned char __bss_start[];
extern unsigned char __bss_end[];

cap_id_t serial_cap_id = CAP_ID_INVALID;

__attribute__((noreturn))
void exit(uintptr_t code) {
	(void)exit_process(code);
	for (;;) {
		(void)exit_thread(code);
	}
}

__attribute__((noreturn))
void _start(uintptr_t arg) {
	memset(__bss_start, 0, __bss_end - __bss_start);
	serial_cap_id = (cap_id_t)arg;
	exit((uintptr_t)main(0, (char**)0));
}
