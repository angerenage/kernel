#include <hal/io_port.h>
#include <stddef.h>
#include <string.h>

void hal_io_port_bitmap_deny_all(struct hal_io_port_bitmap* bitmap) {
	if (bitmap == NULL) return;
	memset(bitmap->bytes, 0xff, sizeof(bitmap->bytes));
}

bool hal_io_port_bitmap_allow(struct hal_io_port_bitmap* bitmap, uint32_t base, uint32_t count) {
	if (bitmap == NULL || count == 0u || base >= HAL_IO_PORT_COUNT || count > HAL_IO_PORT_COUNT - base) return false;

	for (uint32_t port = base; port < base + count; port++) bitmap->bytes[port / 8u] &= (uint8_t)~(1u << (port % 8u));
	return true;
}

bool hal_io_port_bitmap_port_allowed(const struct hal_io_port_bitmap* bitmap, uint32_t port) {
	if (bitmap == NULL || port >= HAL_IO_PORT_COUNT) return false;
	return (bitmap->bytes[port / 8u] & (uint8_t)(1u << (port % 8u))) == 0u;
}
