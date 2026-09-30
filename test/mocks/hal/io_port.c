#include <hal/io_port.h>
#include <stddef.h>
#include <string.h>

static struct hal_io_port_bitmap        loaded_bitmap;
static bool                             loaded_bitmap_present;
static size_t                           load_count;
static size_t                           invalidation_count;
static const struct hal_io_port_bitmap* selected_bitmap;
static uint64_t                         selected_generation;
static bool                             selection_valid;
void                                    hal_io_port_bitmap_deny_all(struct hal_io_port_bitmap* bitmap) {
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

bool hal_io_port_bitmap_load(const struct hal_io_port_bitmap* bitmap) {
	load_count++;
	selection_valid       = false;
	loaded_bitmap_present = bitmap != NULL;
	if (bitmap != NULL) loaded_bitmap = *bitmap;
	else hal_io_port_bitmap_deny_all(&loaded_bitmap);
	return true;
}

bool hal_io_port_bitmap_select(const struct hal_io_port_bitmap* bitmap, uint64_t generation) {
	if (selection_valid && selected_bitmap == bitmap && selected_generation == generation) return true;
	if (!hal_io_port_bitmap_load(bitmap)) return false;
	selected_bitmap     = bitmap;
	selected_generation = generation;
	selection_valid     = true;
	return true;
}

void hal_io_port_bitmap_context_switch(void) {
	(void)hal_io_port_bitmap_load(NULL);
}

void hal_io_port_bitmap_invalidate_all(void) {
	invalidation_count++;
	selection_valid       = false;
	loaded_bitmap_present = false;
	hal_io_port_bitmap_deny_all(&loaded_bitmap);
}

void hal_io_port_mock_reset(void) {
	loaded_bitmap_present = false;
	load_count            = 0u;
	invalidation_count    = 0u;
	selected_bitmap       = NULL;
	selected_generation   = 0u;
	selection_valid       = false;
	hal_io_port_bitmap_deny_all(&loaded_bitmap);
}

const struct hal_io_port_bitmap* hal_io_port_mock_loaded_bitmap(void) {
	return &loaded_bitmap;
}

bool hal_io_port_mock_bitmap_present(void) {
	return loaded_bitmap_present;
}

size_t hal_io_port_mock_load_count(void) {
	return load_count;
}

size_t hal_io_port_mock_invalidation_count(void) {
	return invalidation_count;
}
