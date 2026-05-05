#include <base/display.h>

__attribute__((weak))
void base_display_write(const char* data, size_t length) {
	(void)data;
	(void)length;
}
