#include <base/cap.h>
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
void _start(const cap_id_t* serial_cap) {
	memset(__bss_start, 0, __bss_end - __bss_start);
	serial_cap_id = serial_cap == NULL ? CAP_ID_INVALID : *serial_cap;
	exit((uintptr_t)main(0, (char**)0));
}
