#include <libc/stdlib.h>

int abs(int value) {
	return value < 0 ? -value : value;
}

long labs(long value) {
	return value < 0 ? -value : value;
}
