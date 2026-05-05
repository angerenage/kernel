#include <base/display.h>
#include <hal/serial.h>

void base_display_write(const char* data, size_t length) {
	if (!data) return;

	for (size_t i = 0; i < length; i++) {
		if (data[i] == '\n') {
			hal_serial_write_char('\r');
		}
		hal_serial_write_char(data[i]);
	}
}
