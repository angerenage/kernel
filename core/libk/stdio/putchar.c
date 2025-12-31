#include <stdio.h>
#include <stdio/utils.h>

void putchar(char ch) {
	serial_out_char(ch);
}
