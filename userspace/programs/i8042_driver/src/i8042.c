#include "i8042.h"

#include <base/channel.h>
#include <base/interrupt.h>
#include <base/io_port.h>
#include <base/process.h>
#include <base/signal.h>
#include <protocol/device.h>
#include <protocol/ps2.h>
#include <runtime/device.h>
#include <runtime/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/interrupt.h>
#include <system/io_port.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/time.h>
#include <system/upcall.h>

#define I8042_DATA_RESOURCE "data_port"
#define I8042_COMMAND_RESOURCE "status_command_port"
#define I8042_KEYBOARD_INTERRUPT_RESOURCE "keyboard_interrupt"
#define I8042_MOUSE_INTERRUPT_RESOURCE "mouse_interrupt"
#define ACPI_IO_RESOURCE_0 "io_0"
#define ACPI_IO_RESOURCE_1 "io_1"
#define ACPI_INTERRUPT_RESOURCE_0 "interrupt_0"
#define ACPI_INTERRUPT_RESOURCE_1 "interrupt_1"

#define I8042_DATA_PORT 0x60u
#define I8042_COMMAND_PORT 0x64u

#define I8042_STATUS_OUTPUT_FULL (1u << 0u)
#define I8042_STATUS_INPUT_FULL (1u << 1u)
#define I8042_STATUS_AUXILIARY_DATA (1u << 5u)

#define I8042_CONFIG_KEYBOARD_INTERRUPT (1u << 0u)
#define I8042_CONFIG_MOUSE_INTERRUPT (1u << 1u)
#define I8042_CONFIG_KEYBOARD_DISABLED (1u << 4u)
#define I8042_CONFIG_MOUSE_DISABLED (1u << 5u)
#define I8042_CONFIG_KEYBOARD_TRANSLATION (1u << 6u)

#define I8042_COMMAND_READ_CONFIG 0x20u
#define I8042_COMMAND_WRITE_CONFIG 0x60u
#define I8042_COMMAND_TEST_CONTROLLER 0xaau
#define I8042_COMMAND_TEST_MOUSE 0xa9u
#define I8042_COMMAND_TEST_KEYBOARD 0xabu
#define I8042_COMMAND_DISABLE_MOUSE 0xa7u
#define I8042_COMMAND_ENABLE_MOUSE 0xa8u
#define I8042_COMMAND_DISABLE_KEYBOARD 0xadu
#define I8042_COMMAND_ENABLE_KEYBOARD 0xaeu
#define I8042_COMMAND_WRITE_MOUSE 0xd4u

#define I8042_CONTROLLER_TEST_SUCCESS 0x55u
#define I8042_PORT_TEST_SUCCESS 0x00u
#define PS2_DEVICE_ACKNOWLEDGE 0xfau
#define PS2_DEVICE_RESEND 0xfeu
#define PS2_DEVICE_RESET 0xffu
#define PS2_DEVICE_DISABLE_SCANNING 0xf5u
#define PS2_DEVICE_SELF_TEST_SUCCESS 0xaau

#define I8042_COMMAND_ATTEMPTS 3u
#define I8042_RESPONSE_LIMIT 4u
#define I8042_DRAIN_LIMIT 256u
#define I8042_QUEUE_CAPACITY 256u
#define I8042_POLL_INTERVAL_MS 1u
#define I8042_IO_TIMEOUT_MS 50u
#define PS2_RESET_TIMEOUT_MS 500u
#define NANOSECONDS_PER_MILLISECOND 1000000ull

#define I8042_KEYBOARD_OBJECT_ID 1u
#define I8042_MOUSE_OBJECT_ID 2u

#define I8042_PORT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define I8042_INTERRUPT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE))
#define I8042_TRANSPORT_PROVIDER_RIGHTS (PS2_TRANSPORT_DRIVER_RIGHTS | CAP_DELEGATE)

/* One controller port exported through a controller-independent PS/2 transport. */
struct i8042_ps2_port {
	bool     auxiliary;
	bool     present;
	bool     interrupt_bound;
	bool     transport_published;
	cap_id_t interrupt_cap;
	cap_id_t interrupt_signal_cap;
	cap_id_t receive_signal_cap;
	cap_id_t transport_cap;
	uint64_t object_id;
	uint8_t  queue[I8042_QUEUE_CAPACITY];
	size_t   head;
	size_t   tail;
	bool     overrun;
	bool     notify_pending;
};

/* Capabilities, transport objects, and hardware state for one controller process. */
struct i8042 {
	cap_id_t              data_cap;
	cap_id_t              command_cap;
	bool                  data_mapped;
	bool                  command_mapped;
	channel_id_t          endpoint;
	cap_id_t              activity_cap;
	process_id_t          pid;
	struct i8042_ps2_port keyboard;
	struct i8042_ps2_port mouse;
};

/* Flexible protocol response backed by the largest permitted read payload. */
struct i8042_read_response {
	uint32_t size;
	uint32_t flags;
	uint8_t  bytes[PS2_TRANSPORT_MAX_READ];
};

static struct i8042* active_controller;
static uint8_t       request_buffer[CAP_MAX_REQUEST_SIZE];

static void report_error(const char* message) {
	(void)fwrite(message, 1u, strlen(message), stderr);
}

static uint8_t read_status(void) {
	return io_read8(I8042_COMMAND_PORT);
}

static bool deadline_after_ms(uint64_t timeout_ms, uint64_t* out_deadline) {
	uint64_t now;
	uint64_t duration;

	if (out_deadline == NULL || timeout_ms == 0u || timeout_ms > UINT64_MAX / NANOSECONDS_PER_MILLISECOND ||
	    monotonic_time_ns(&now) != SYSCALL_STATUS_OK)
		return false;
	duration      = timeout_ms * NANOSECONDS_PER_MILLISECOND;
	*out_deadline = now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
	return true;
}

static bool wait_until_deadline(uint64_t deadline) {
	uint64_t now;

	if (monotonic_time_ns(&now) != SYSCALL_STATUS_OK || now >= deadline) return false;
	return sleep_ms(I8042_POLL_INTERVAL_MS) == SYSCALL_STATUS_OK;
}

static bool wait_input_empty(void) {
	uint64_t deadline;

	if (!deadline_after_ms(I8042_IO_TIMEOUT_MS, &deadline)) return false;
	for (;;) {
		if ((read_status() & I8042_STATUS_INPUT_FULL) == 0u) return true;
		if (!wait_until_deadline(deadline)) return false;
	}
}

static bool write_command(uint8_t command) {
	if (!wait_input_empty()) return false;
	io_write8(I8042_COMMAND_PORT, command);
	return true;
}

static bool write_data(uint8_t value) {
	if (!wait_input_empty()) return false;
	io_write8(I8042_DATA_PORT, value);
	return true;
}

static bool write_device(bool auxiliary, uint8_t value) {
	if (auxiliary && !write_command(I8042_COMMAND_WRITE_MOUSE)) return false;
	return write_data(value);
}

static bool read_output_timeout(bool auxiliary, uint64_t timeout_ms, uint8_t* out_value) {
	uint64_t deadline;

	if (out_value == NULL || !deadline_after_ms(timeout_ms, &deadline)) return false;
	for (;;) {
		uint8_t status = read_status();
		if ((status & I8042_STATUS_OUTPUT_FULL) != 0u) {
			uint8_t value = io_read8(I8042_DATA_PORT);
			if (((status & I8042_STATUS_AUXILIARY_DATA) != 0u) == auxiliary) {
				*out_value = value;
				return true;
			}
		}
		if (!wait_until_deadline(deadline)) return false;
	}
}

static bool read_output(bool auxiliary, uint8_t* out_value) {
	return read_output_timeout(auxiliary, I8042_IO_TIMEOUT_MS, out_value);
}

static bool flush_output(void) {
	for (size_t count = 0u; count < I8042_DRAIN_LIMIT; count++) {
		if ((read_status() & I8042_STATUS_OUTPUT_FULL) == 0u) return true;
		(void)io_read8(I8042_DATA_PORT);
	}
	return (read_status() & I8042_STATUS_OUTPUT_FULL) == 0u;
}

static bool controller_command_response(uint8_t command, uint8_t* out_value) {
	return write_command(command) && read_output(false, out_value);
}

static bool read_config(uint8_t* out_config) {
	return controller_command_response(I8042_COMMAND_READ_CONFIG, out_config);
}

static bool write_config(uint8_t config) {
	return write_command(I8042_COMMAND_WRITE_CONFIG) && write_data(config);
}

static bool probe_command(bool auxiliary, uint8_t command) {
	for (size_t attempt = 0u; attempt < I8042_COMMAND_ATTEMPTS; attempt++) {
		if (!write_device(auxiliary, command)) return false;
		for (size_t response_index = 0u; response_index < I8042_RESPONSE_LIMIT; response_index++) {
			uint8_t response;
			if (!read_output(auxiliary, &response)) return false;
			if (response == PS2_DEVICE_ACKNOWLEDGE) return true;
			if (response == PS2_DEVICE_RESEND) break;
		}
	}
	return false;
}

static bool reset_for_probe(bool auxiliary) {
	uint8_t response;
	if (!probe_command(auxiliary, PS2_DEVICE_RESET) ||
	    !read_output_timeout(auxiliary, PS2_RESET_TIMEOUT_MS, &response) || response != PS2_DEVICE_SELF_TEST_SUCCESS)
		return false;
	/* A mouse ID following BAT is ignored by probe_command while it waits for the disable ACK. */
	return probe_command(auxiliary, PS2_DEVICE_DISABLE_SCANNING);
}

static bool map_port(cap_id_t cap, uint32_t expected_port) {
	struct io_port_info info;
	if (io_port_info(cap, &info) != SYSCALL_STATUS_OK || info.base != expected_port || info.count != 1u) return false;
	return io_port_map(cap) == SYSCALL_STATUS_OK;
}

static bool validate_interrupt(cap_id_t cap) {
	struct interrupt_info info;
	return interrupt_info(cap, &info) == SYSCALL_STATUS_OK && info.kind == INTERRUPT_KIND_SOURCE && !info.bound;
}

static bool acquire_resource(cap_id_t device_cap, const char* name, cap_rights_t rights, cap_id_t* out_cap) {
	return device_resource_acquire(device_cap, name, strlen(name), rights, out_cap) == SYSCALL_STATUS_OK;
}

static void drop_resource(cap_id_t* cap) {
	if (*cap != CAP_ID_INVALID) (void)cap_drop(*cap);
	*cap = CAP_ID_INVALID;
}

static bool acquire_ports(cap_id_t device_cap, struct i8042* controller) {
	struct io_port_info first;
	struct io_port_info second;
	cap_id_t            first_cap  = CAP_ID_INVALID;
	cap_id_t            second_cap = CAP_ID_INVALID;

	if (acquire_resource(device_cap, I8042_DATA_RESOURCE, I8042_PORT_RIGHTS, &controller->data_cap) &&
	    acquire_resource(device_cap, I8042_COMMAND_RESOURCE, I8042_PORT_RIGHTS, &controller->command_cap))
		return true;
	drop_resource(&controller->command_cap);
	drop_resource(&controller->data_cap);
	if (!acquire_resource(device_cap, ACPI_IO_RESOURCE_0, I8042_PORT_RIGHTS, &first_cap) ||
	    !acquire_resource(device_cap, ACPI_IO_RESOURCE_1, I8042_PORT_RIGHTS, &second_cap) ||
	    io_port_info(first_cap, &first) != SYSCALL_STATUS_OK ||
	    io_port_info(second_cap, &second) != SYSCALL_STATUS_OK || first.count != 1u || second.count != 1u ||
	    !((first.base == I8042_DATA_PORT && second.base == I8042_COMMAND_PORT) ||
	      (first.base == I8042_COMMAND_PORT && second.base == I8042_DATA_PORT))) {
		drop_resource(&second_cap);
		drop_resource(&first_cap);
		return false;
	}
	if (first.base == I8042_DATA_PORT) {
		controller->data_cap    = first_cap;
		controller->command_cap = second_cap;
	}
	else {
		controller->data_cap    = second_cap;
		controller->command_cap = first_cap;
	}
	return true;
}

static bool acquire_resources(cap_id_t device_cap, struct i8042* controller) {
	struct device_info info;
	if (device_get_info(device_cap, &info) != SYSCALL_STATUS_OK || info.resource_count < 3u ||
	    !acquire_ports(device_cap, controller))
		return false;
	if (!acquire_resource(device_cap,
	                      I8042_KEYBOARD_INTERRUPT_RESOURCE,
	                      I8042_INTERRUPT_RIGHTS,
	                      &controller->keyboard.interrupt_cap) &&
	    !acquire_resource(
			device_cap, ACPI_INTERRUPT_RESOURCE_0, I8042_INTERRUPT_RIGHTS, &controller->keyboard.interrupt_cap))
		return false;
	if (!acquire_resource(
			device_cap, I8042_MOUSE_INTERRUPT_RESOURCE, I8042_INTERRUPT_RIGHTS, &controller->mouse.interrupt_cap))
		(void)acquire_resource(
			device_cap, ACPI_INTERRUPT_RESOURCE_1, I8042_INTERRUPT_RIGHTS, &controller->mouse.interrupt_cap);
	if (!validate_interrupt(controller->keyboard.interrupt_cap) ||
	    (controller->mouse.interrupt_cap != CAP_ID_INVALID && !validate_interrupt(controller->mouse.interrupt_cap)))
		return false;
	controller->data_mapped = map_port(controller->data_cap, I8042_DATA_PORT);
	if (!controller->data_mapped) return false;
	controller->command_mapped = map_port(controller->command_cap, I8042_COMMAND_PORT);
	return controller->command_mapped;
}

static bool prepare_controller(uint8_t* out_config, bool* out_mouse_port) {
	uint8_t config;
	uint8_t response;

	if (!write_command(I8042_COMMAND_DISABLE_KEYBOARD) || !write_command(I8042_COMMAND_DISABLE_MOUSE) ||
	    !flush_output() || !read_config(&config))
		return false;
	config &=
		(uint8_t)~(I8042_CONFIG_KEYBOARD_INTERRUPT | I8042_CONFIG_MOUSE_INTERRUPT | I8042_CONFIG_KEYBOARD_TRANSLATION);
	config |= I8042_CONFIG_KEYBOARD_DISABLED | I8042_CONFIG_MOUSE_DISABLED;
	if (!write_config(config) || !controller_command_response(I8042_COMMAND_TEST_CONTROLLER, &response) ||
	    response != I8042_CONTROLLER_TEST_SUCCESS)
		return false;

	/* A successful controller self-test may restore firmware defaults. */
	if (!write_command(I8042_COMMAND_DISABLE_KEYBOARD) || !write_command(I8042_COMMAND_DISABLE_MOUSE) ||
	    !write_config(config) || !write_command(I8042_COMMAND_ENABLE_MOUSE) || !read_config(&response))
		return false;
	*out_mouse_port = (response & I8042_CONFIG_MOUSE_DISABLED) == 0u;
	if (!write_command(I8042_COMMAND_DISABLE_MOUSE)) return false;
	*out_config = config;
	return true;
}

static bool probe_device(bool auxiliary) {
	uint8_t response;
	uint8_t test_command    = auxiliary ? I8042_COMMAND_TEST_MOUSE : I8042_COMMAND_TEST_KEYBOARD;
	uint8_t enable_command  = auxiliary ? I8042_COMMAND_ENABLE_MOUSE : I8042_COMMAND_ENABLE_KEYBOARD;
	uint8_t disable_command = auxiliary ? I8042_COMMAND_DISABLE_MOUSE : I8042_COMMAND_DISABLE_KEYBOARD;

	if (!controller_command_response(test_command, &response) || response != I8042_PORT_TEST_SUCCESS) return false;
	if (!write_command(enable_command) || !reset_for_probe(auxiliary)) {
		(void)write_command(disable_command);
		return false;
	}
	return write_command(disable_command);
}

static void queue_byte(struct i8042_ps2_port* port, uint8_t value) {
	size_t head = __atomic_load_n(&port->head, __ATOMIC_RELAXED);
	size_t next = (head + 1u) % I8042_QUEUE_CAPACITY;
	if (next == __atomic_load_n(&port->tail, __ATOMIC_ACQUIRE))
		__atomic_store_n(&port->overrun, true, __ATOMIC_RELEASE);
	else {
		port->queue[head] = value;
		__atomic_store_n(&port->head, next, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&port->notify_pending, true, __ATOMIC_RELEASE);
}

static void drain_interrupt_data(void) {
	struct i8042* controller = active_controller;
	if (controller == NULL) return;
	for (size_t count = 0u; count < I8042_DRAIN_LIMIT; count++) {
		uint8_t status = read_status();
		if ((status & I8042_STATUS_OUTPUT_FULL) == 0u) return;
		uint8_t                value = io_read8(I8042_DATA_PORT);
		struct i8042_ps2_port* port =
			(status & I8042_STATUS_AUXILIARY_DATA) != 0u ? &controller->mouse : &controller->keyboard;
		if (port->present) queue_byte(port, value);
	}
}

DEFINE_UPCALL_HANDLER(i8042_interrupt, sender, arg0, arg1, arg2, arg3) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	if ((process_id_t)sender == SIGNAL_SENDER_KERNEL) drain_interrupt_data();
}

static bool bind_interrupt(struct i8042_ps2_port* port) {
	if (!port->present || port->interrupt_cap == CAP_ID_INVALID) return true;
	if (signal_create(&port->interrupt_signal_cap) != SYSCALL_STATUS_OK ||
	    signal_set_handler(port->interrupt_signal_cap, i8042_interrupt, SIGNAL_HANDLER_FLAG_NONE) !=
	        SYSCALL_STATUS_OK ||
	    interrupt_bind(port->interrupt_cap, port->interrupt_signal_cap) != SYSCALL_STATUS_OK)
		return false;
	port->interrupt_bound = true;
	return true;
}

static bool start_controller(struct i8042* controller, uint8_t config) {
	if (controller->keyboard.present) {
		config |= I8042_CONFIG_KEYBOARD_INTERRUPT;
		config &= (uint8_t)~I8042_CONFIG_KEYBOARD_DISABLED;
	}
	if (controller->mouse.present) {
		if (controller->mouse.interrupt_cap != CAP_ID_INVALID) config |= I8042_CONFIG_MOUSE_INTERRUPT;
		else config &= (uint8_t)~I8042_CONFIG_MOUSE_INTERRUPT;
		config &= (uint8_t)~I8042_CONFIG_MOUSE_DISABLED;
	}
	return write_config(config);
}

static struct i8042_ps2_port* find_port(struct i8042* controller, uint64_t object_id) {
	if (controller->keyboard.present && object_id == controller->keyboard.object_id) return &controller->keyboard;
	if (controller->mouse.present && object_id == controller->mouse.object_id) return &controller->mouse;
	return NULL;
}

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static bool dispatch_read(struct i8042_ps2_port* port, const struct cap_request* call, const void* data) {
	struct ps2_transport_read_request request;
	struct i8042_read_response        response = {0};
	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.max_size == 0u || request.max_size > PS2_TRANSPORT_MAX_READ ||
	    call->response_capacity < sizeof(struct ps2_transport_read_response) + request.max_size)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);

	size_t tail = __atomic_load_n(&port->tail, __ATOMIC_RELAXED);
	size_t head = __atomic_load_n(&port->head, __ATOMIC_ACQUIRE);
	while (tail != head && response.size < request.max_size) {
		response.bytes[response.size++] = port->queue[tail];
		tail                            = (tail + 1u) % I8042_QUEUE_CAPACITY;
	}
	bool overrun = __atomic_exchange_n(&port->overrun, false, __ATOMIC_ACQ_REL);
	if (overrun) response.flags |= PS2_TRANSPORT_READ_FLAG_OVERRUN;
	if (response.size == 0u && !overrun) return reply(call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE);
	if (!reply(call, &response, sizeof(struct ps2_transport_read_response) + response.size, SYSCALL_STATUS_OK)) {
		if (overrun) __atomic_store_n(&port->overrun, true, __ATOMIC_RELEASE);
		return false;
	}
	__atomic_store_n(&port->tail, tail, __ATOMIC_RELEASE);
	return true;
}

static bool dispatch_write(struct i8042_ps2_port* port, const struct cap_request* call, const void* data) {
	struct ps2_transport_write_request request;
	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	if (request.reserved[0] != 0u || request.reserved[1] != 0u || request.reserved[2] != 0u)
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply(
		call, NULL, 0u, write_device(port->auxiliary, request.value) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED);
}

static bool dispatch_acquire_signal(struct i8042_ps2_port* port, const struct cap_request* call) {
	struct ps2_transport_acquire_signal_response response = {.capability = CAP_ID_INVALID};
	if ((call->rights & CAP_READ) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(struct ps2_transport_acquire_signal_request) ||
	    call->response_capacity < sizeof(response))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	syscall_status_t status = cap_delegate(port->receive_signal_cap, call->caller, CAP_WAIT, &response.capability);
	if (status != SYSCALL_STATUS_OK) return reply(call, NULL, 0u, status);
	if (reply(call, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(response.capability, 0u);
	return false;
}

static bool dispatch_transport(struct i8042* controller, const struct cap_request* call, const void* data) {
	struct ps2_transport_request_header header;
	struct i8042_ps2_port*              port = find_port(controller, call->object_id);
	if (port == NULL || data == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	switch (header.op) {
	case PS2_TRANSPORT_OP_READ:
		return dispatch_read(port, call, data);
	case PS2_TRANSPORT_OP_WRITE:
		return dispatch_write(port, call, data);
	case PS2_TRANSPORT_OP_ACQUIRE_RECEIVE_SIGNAL:
		return dispatch_acquire_signal(port, call);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool initialize_transport(struct i8042* controller, struct i8042_ps2_port* port) {
	if (!port->present) return true;
	if (signal_create(&port->receive_signal_cap) != SYSCALL_STATUS_OK) return false;
	if (cap_publish(controller->endpoint,
	                port->object_id,
	                controller->pid,
	                I8042_TRANSPORT_PROVIDER_RIGHTS,
	                &port->transport_cap) != SYSCALL_STATUS_OK)
		return false;
	port->transport_published = true;
	return true;
}

static bool create_child(cap_id_t device_cap, const struct i8042_ps2_port* port, const char* name, size_t name_size) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	syscall_status_t      status  = device_builder_begin_child(device_cap, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, name_size);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, PS2_COMPATIBLE_DEVICE, sizeof(PS2_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
		                                     PS2_TRANSPORT_RESOURCE,
		                                     sizeof(PS2_TRANSPORT_RESOURCE) - 1u,
		                                     port->transport_cap,
		                                     PS2_TRANSPORT_DRIVER_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static bool initialize_service(struct i8042* controller, cap_id_t device_cap) {
	if (channel_create(&controller->endpoint, &controller->activity_cap) != SYSCALL_STATUS_OK) return false;
	if (!initialize_transport(controller, &controller->keyboard) ||
	    !initialize_transport(controller, &controller->mouse))
		return false;
	if (controller->keyboard.present &&
	    !create_child(device_cap, &controller->keyboard, "keyboard_port", sizeof("keyboard_port") - 1u))
		return false;
	if (controller->mouse.present &&
	    !create_child(device_cap, &controller->mouse, "auxiliary_port", sizeof("auxiliary_port") - 1u))
		return false;
	return true;
}

static bool send_notification(struct i8042_ps2_port* port) {
	if (!port->present || !__atomic_exchange_n(&port->notify_pending, false, __ATOMIC_ACQ_REL)) return true;
	struct signal_send_response response;
	if (signal_send(port->receive_signal_cap, 0u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) ==
	    SYSCALL_STATUS_OK)
		return true;
	__atomic_store_n(&port->notify_pending, true, __ATOMIC_RELEASE);
	return false;
}

static bool serve(struct i8042* controller) {
	/* ACPI commonly places IRQ12 on a separate PNP0Fxx node instead of the PNP0303 controller node. */
	bool poll = controller->mouse.present && controller->mouse.interrupt_cap == CAP_ID_INVALID;

	for (;;) {
		struct cap_request call;
		bool               received;
		syscall_status_t   status;
		do {
			received = false;
			status   = channel_recv(controller->endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received && !dispatch_transport(controller, &call, request_buffer)) return false;
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(controller->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return false;
		} while (received);
		if (poll) drain_interrupt_data();
		if (!send_notification(&controller->keyboard) || !send_notification(&controller->mouse)) return false;

		if (poll) {
			if (sleep_ms(I8042_POLL_INTERVAL_MS) != SYSCALL_STATUS_OK) return false;
		}
		else {
			struct signal_message activity;
			status = signal_wait(controller->activity_cap, &activity);
			if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
		}
	}
}

static void cleanup_port(struct i8042* controller, struct i8042_ps2_port* port) {
	if (port->interrupt_bound) (void)interrupt_unbind(port->interrupt_cap);
	if (port->interrupt_signal_cap != CAP_ID_INVALID) (void)signal_destroy(port->interrupt_signal_cap);
	if (port->transport_published) (void)cap_unpublish(controller->endpoint, port->object_id);
	if (port->receive_signal_cap != CAP_ID_INVALID) (void)signal_destroy(port->receive_signal_cap);
	if (port->interrupt_cap != CAP_ID_INVALID) (void)cap_drop(port->interrupt_cap);
}

static void cleanup(struct i8042* controller) {
	active_controller = NULL;
	cleanup_port(controller, &controller->mouse);
	cleanup_port(controller, &controller->keyboard);
	if (controller->activity_cap != CAP_ID_INVALID) (void)cap_drop(controller->activity_cap);
	if (controller->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(controller->endpoint);
	if (controller->command_mapped) (void)io_port_unmap(controller->command_cap);
	if (controller->data_mapped) (void)io_port_unmap(controller->data_cap);
	if (controller->command_cap != CAP_ID_INVALID) (void)cap_drop(controller->command_cap);
	if (controller->data_cap != CAP_ID_INVALID) (void)cap_drop(controller->data_cap);
}

enum device_driver_result i8042_driver_run(cap_id_t device_cap) {
	struct i8042 controller = {
		.data_cap     = CAP_ID_INVALID,
		.command_cap  = CAP_ID_INVALID,
		.endpoint     = CHANNEL_ID_INVALID,
		.activity_cap = CAP_ID_INVALID,
		.pid          = PROCESS_PID_INVALID,
		.keyboard     = {.interrupt_cap        = CAP_ID_INVALID,
		                 .interrupt_signal_cap = CAP_ID_INVALID,
		                 .receive_signal_cap   = CAP_ID_INVALID,
		                 .transport_cap        = CAP_ID_INVALID,
		                 .object_id            = I8042_KEYBOARD_OBJECT_ID},
		.mouse        = {.auxiliary            = true,
		                 .interrupt_cap        = CAP_ID_INVALID,
		                 .interrupt_signal_cap = CAP_ID_INVALID,
		                 .receive_signal_cap   = CAP_ID_INVALID,
		                 .transport_cap        = CAP_ID_INVALID,
		                 .object_id            = I8042_MOUSE_OBJECT_ID},
	};
	struct self_info          self;
	uint8_t                   config;
	bool                      mouse_port;
	enum device_driver_result result = DEVICE_DRIVER_ERROR;

	if (process_self_info(&self) != SYSCALL_STATUS_OK) {
		report_error("i8042: could not identify the controller process\n");
		goto done;
	}
	controller.pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (!acquire_resources(device_cap, &controller)) {
		report_error("i8042: could not acquire the controller resources\n");
		goto done;
	}
	if (!prepare_controller(&config, &mouse_port)) {
		report_error("i8042: controller initialization failed\n");
		goto done;
	}
	controller.keyboard.present = probe_device(false);
	controller.mouse.present    = mouse_port && probe_device(true);
	if (!controller.keyboard.present && !controller.mouse.present) {
		RUNTIME_DEBUG_ERROR("i8042: no populated PS/2 ports\n");
		result = DEVICE_DRIVER_UNSUPPORTED;
		goto done;
	}
	if (!flush_output() || !initialize_service(&controller, device_cap)) {
		report_error("i8042: could not publish PS/2 child transports\n");
		goto done;
	}
	active_controller = &controller;
	if (!bind_interrupt(&controller.keyboard) || !bind_interrupt(&controller.mouse) ||
	    !start_controller(&controller, config)) {
		report_error("i8042: interrupt or controller startup failed\n");
		goto done;
	}
	printf("i8042: controller online (primary=%s, auxiliary=%s)\n",
	       controller.keyboard.present ? "yes" : "no",
	       controller.mouse.present ? "yes" : "no");
	(void)serve(&controller);

done:
	cleanup(&controller);
	return result;
}
