#include <base/display.h>
#include <syscall.h>

void base_display_write(const char* data, size_t length) {
	(void)print(data, length);
}
