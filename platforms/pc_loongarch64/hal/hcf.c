#include "hal/hcf.h"

#include <stdio.h>

__attribute__((noreturn))
void hcf(void) {
	printf("kernel: hcf\n");
	hcf_silent();
}

__attribute__((noreturn))
void hcf_silent(void) {
	for (;;) {
		__asm__ volatile("idle 0" ::: "memory");
	}
}
