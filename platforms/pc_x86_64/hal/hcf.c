#include "hal/hcf.h"

#include <stdio.h>

__attribute__((noreturn))
void hcf(void) {
	printf("kernel: hcf\n");
	hcf_silent();
}

__attribute__((noreturn))
void hcf_silent(void) {
	__asm__ volatile("cli");
	for (;;) {
		__asm__ volatile("hlt");
	}
}
