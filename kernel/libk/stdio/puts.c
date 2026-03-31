#include <stdio.h>

void putchar(char ch);

void puts(const char* str) {
	if (!str) return;

	while (*str) putchar(*str++);
	putchar('\n');
}
