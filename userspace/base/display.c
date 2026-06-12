#include <base/cap.h>
#include <base/display.h>
#include <syscall.h>

extern cap_id_t serial_cap_id;

void base_display_write(const char* data, size_t length) {
	if (serial_cap_id == CAP_ID_INVALID) return;
	(void)cap_call(serial_cap_id, data, length);
}
