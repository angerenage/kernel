#include "hal/hcf.h"

__attribute__((noreturn))
void hcf(void) {
	for (;;) {
		__asm__ volatile("" ::: "memory");
	}
}
