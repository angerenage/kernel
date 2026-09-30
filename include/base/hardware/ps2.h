#pragma once

#include <stdint.h>

/* Hardware interface used to access a PS/2 controller. */
enum ps2_controller_interface {
	PS2_CONTROLLER_INTERFACE_INVALID = 0,
	PS2_CONTROLLER_INTERFACE_I8042_IO_PORT,
	PS2_CONTROLLER_INTERFACE_COUNT,
};

/* Register ports exposed by an i8042-compatible controller. */
struct ps2_i8042_io_port_interface {
	uint16_t data_port;
	uint16_t status_command_port;
};

/* Interface-specific access information, padded for future interface types. */
union ps2_controller_access {
	struct ps2_i8042_io_port_interface i8042_io_port;
	uint64_t                           reserved[2];
};

/* Source-neutral description of one PS/2 controller. */
struct ps2_controller {
	enum ps2_controller_interface interface;
	uint32_t                      reserved;
	union ps2_controller_access   access;
};
