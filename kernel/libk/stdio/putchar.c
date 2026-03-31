#include <hal/serial.h>
#include <stdio.h>

void putchar(char ch) {
	if (ch == '\n') {
		hal_serial_write_char('\r');
	}

	hal_serial_write_char(ch);
}
