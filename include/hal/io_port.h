#pragma once

#if !defined(PLATFORM_PC_X86_64)
#error "I/O port access is only available on x86_64"
#endif

#include <stdbool.h>
#include <stdint.h>

#define HAL_IO_PORT_COUNT 65536u
#define HAL_IO_PORT_BITMAP_SIZE (HAL_IO_PORT_COUNT / 8u)
#define HAL_IO_PORT_BITMAP_STORAGE_SIZE (HAL_IO_PORT_BITMAP_SIZE + 1u)

/* Complete architectural representation, including the mandatory denied terminator byte. */
struct hal_io_port_bitmap {
	uint8_t bytes[HAL_IO_PORT_BITMAP_STORAGE_SIZE];
};

/* Clear an I/O port bitmap, denying all ports. */
void hal_io_port_bitmap_deny_all(struct hal_io_port_bitmap* bitmap);

/* Allow a range of I/O ports. */
bool hal_io_port_bitmap_allow(struct hal_io_port_bitmap* bitmap, uint32_t base, uint32_t count);

/* Check if a specific I/O port is allowed. */
bool hal_io_port_bitmap_port_allowed(const struct hal_io_port_bitmap* bitmap, uint32_t port);

/* Install permissions directly in the current CPU's already-loaded TSS. */
bool hal_io_port_bitmap_load(const struct hal_io_port_bitmap* bitmap);

/* Select permissions by opaque bitmap identity and generation, avoiding redundant TSS copies. */
bool hal_io_port_bitmap_select(const struct hal_io_port_bitmap* bitmap, uint64_t generation);

/* Reset permissions and cached selection during a hardware thread-context switch. */
void hal_io_port_bitmap_context_switch(void);

/* Force every online CPU through the kernel and invalidate its loaded-bitmap cache. */
void hal_io_port_bitmap_invalidate_all(void);
