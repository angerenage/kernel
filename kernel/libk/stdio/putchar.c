#include <stdarg.h>

#include <stdio.h>

#include "utils.h"

void putchar(char ch) {
	serial_out_char(ch);
}
