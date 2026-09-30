#pragma once

#if !defined(__x86_64__) && !defined(PLATFORM_PC_X86_64) && !defined(IO_PORT_TEST)
#error "I/O-port access is only available on x86_64"
#endif

#include <base/cap.h>
#include <stdint.h>

#define IO_PORT_COUNT 65536u

enum io_port_op {
	IO_PORT_OP_INFO = 0,
	IO_PORT_OP_DERIVE,
	IO_PORT_OP_MAP,
	IO_PORT_OP_UNMAP,
	IO_PORT_OP_READ,
	IO_PORT_OP_WRITE,
};

enum io_port_width {
	IO_PORT_WIDTH_8  = 1u,
	IO_PORT_WIDTH_16 = 2u,
	IO_PORT_WIDTH_32 = 4u,
};

/* Common header for I/O-port capability requests. */
struct io_port_request_header {
	enum io_port_op op;
};

/* Empty request used for info, map, and unmap operations. */
struct io_port_simple_request {
	struct io_port_request_header header;
};

/* Public half-open range represented by an I/O-port capability. */
struct io_port_info {
	uint32_t base;
	uint32_t count;
};

/* Request a contained child range with no additional rights. */
struct io_port_derive_request {
	struct io_port_request_header header;
	uint32_t                      offset;
	uint32_t                      count;
	cap_rights_t                  rights;
};

/* Capability returned for a derived I/O-port range. */
struct io_port_derive_response {
	cap_id_t io_port_cap;
};

/* Request one syscall-backed port read at an offset within the range. */
struct io_port_read_request {
	struct io_port_request_header header;
	uint32_t                      offset;
	enum io_port_width            width;
};

/* Zero-extended value returned by a syscall-backed port read. */
struct io_port_read_response {
	uint32_t value;
};

/* Request one syscall-backed port write at an offset within the range. */
struct io_port_write_request {
	struct io_port_request_header header;
	uint32_t                      offset;
	enum io_port_width            width;
	uint32_t                      value;
};

/* Read one byte from an I/O port. */
uint8_t io_read8(uint16_t port);

/* Read one word from an I/O port. */
uint16_t io_read16(uint16_t port);

/* Read one doubleword from an I/O port. */
uint32_t io_read32(uint16_t port);

/* Write one byte to an I/O port. */
void io_write8(uint16_t port, uint8_t value);

/* Write one word to an I/O port. */
void io_write16(uint16_t port, uint16_t value);

/* Write one doubleword to an I/O port. */
void io_write32(uint16_t port, uint32_t value);
