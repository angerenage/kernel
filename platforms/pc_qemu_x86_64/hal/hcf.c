#include "hal/hcf.h"

#include <stdio.h>

__attribute__((noreturn))
void hcf(void) {
	printf("kernel: hcf\n");
	__asm__ volatile("cli");
	for (;;) {
		__asm__ volatile("hlt");
	}
}
