#include <base/display.h>
#include <system/display.h>

/* Userspace shim for the libc. */
void base_display_write(const char* data, size_t length) {
	(void)display_write(data, length);
}
