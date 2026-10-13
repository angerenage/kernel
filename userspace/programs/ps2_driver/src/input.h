#pragma once

#include <base/syscall.h>
#include <protocol/input.h>
#include <stddef.h>
#include <stdint.h>

/* One normalized control value supplied while constructing a producer frame. */
struct ps2_input_value {
	uint32_t control_id;
	int64_t  value;
};

/* Opaque input-protocol server owned for the lifetime of the PS/2 process. */
struct ps2_input_server;

/* Create and advertise the PS/2 process's input service. */
syscall_status_t ps2_input_server_create(const char* service, enum input_device_class device_class,
                                         const struct input_control_descriptor* descriptors, size_t descriptor_count,
                                         struct ps2_input_server** out_server);

/* Append one nonempty coherent control frame and notify waiting clients. */
syscall_status_t ps2_input_server_publish(struct ps2_input_server* server, const struct ps2_input_value* values,
                                          size_t count);

/* Append a state-reset frame after transport loss or device reinitialization. */
syscall_status_t ps2_input_server_reset(struct ps2_input_server* server);
