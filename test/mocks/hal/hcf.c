#include <stdio.h>
#include <stdlib.h>

__attribute__((noreturn))
void hcf(void) {
	fputs("hcf: unrecoverable assertion failure\n", stderr);
	abort();
}
