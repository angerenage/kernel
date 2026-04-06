#include "hal/hcf.h"

#include <stdio.h>

__attribute__((noreturn))
void hcf(void) {
	printf("kernel: hcf\n");
	for (;;) {
		__asm__ volatile("" ::: "memory");
	}
}
