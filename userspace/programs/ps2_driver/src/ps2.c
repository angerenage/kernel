#include "ps2.h"

#include <protocol/device.h>
#include <protocol/ps2.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/signal.h>
#include <system/time.h>

#include "events.h"
#include "keyboard.h"
#include "mouse.h"

#define PS2_ACK 0xfau
#define PS2_RESEND 0xfeu
#define PS2_SELF_TEST_SUCCESS 0xaau

#define PS2_COMMAND_RESET 0xffu
#define PS2_COMMAND_IDENTIFY 0xf2u
#define PS2_COMMAND_ENABLE_SCANNING 0xf4u
#define PS2_COMMAND_DISABLE_SCANNING 0xf5u
#define PS2_COMMAND_SET_SCAN_CODE_SET 0xf0u
#define PS2_COMMAND_SET_SAMPLE_RATE 0xf3u

#define PS2_SCAN_CODE_SET_2 0x02u
#define PS2_MOUSE_ID_STANDARD 0x00u
#define PS2_MOUSE_ID_WHEEL 0x03u
#define PS2_MOUSE_ID_FIVE_BUTTON 0x04u

#define PS2_RESPONSE_POLL_LIMIT 4000u
#define PS2_OPTIONAL_ID_POLL_LIMIT 320u
#define PS2_COMMAND_ATTEMPTS 3u
#define PS2_UNEXPECTED_RESPONSE_LIMIT 16u
#define PS2_SERVICE_READ_SIZE 64u

/* Result of waiting for one byte from a nonblocking transport. */
enum ps2_receive_result {
	PS2_RECEIVE_OK = 0u,
	PS2_RECEIVE_TIMEOUT,
	PS2_RECEIVE_OVERRUN,
	PS2_RECEIVE_FAILED,
};

/* Runtime state owned by one independently supervised PS/2 driver. */
struct ps2_device {
	cap_id_t             transport_cap;
	cap_id_t             receive_signal_cap;
	enum ps2_device_kind kind;
	uint8_t              mouse_id;
};

struct ps2_read_buffer {
	uint32_t size;
	uint32_t flags;
	uint8_t  bytes[PS2_TRANSPORT_MAX_READ];
};

static syscall_status_t transport_read(cap_id_t transport_cap, void* buffer, size_t capacity, size_t* out_size,
                                       bool* out_overrun) {
	const struct ps2_transport_read_request request = {
		.header   = {.op = PS2_TRANSPORT_OP_READ},
		.max_size = (uint32_t)capacity,
	};
	struct ps2_read_buffer response = {0};
	size_t                 response_size;
	syscall_status_t       status;

	if (transport_cap == CAP_ID_INVALID || buffer == NULL || capacity == 0u || capacity > PS2_TRANSPORT_MAX_READ ||
	    out_size == NULL || out_overrun == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_size    = 0u;
	*out_overrun = false;
	status       = cap_call(transport_cap,
	                        &request,
	                        sizeof(request),
	                        &response,
	                        sizeof(struct ps2_transport_read_response) + capacity,
	                        &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size < sizeof(struct ps2_transport_read_response) || response.size > capacity ||
	    response_size != sizeof(struct ps2_transport_read_response) + response.size ||
	    (response.flags & ~(uint32_t)PS2_TRANSPORT_READ_FLAG_OVERRUN) != 0u)
		return SYSCALL_STATUS_FAILED;
	if (response.size != 0u) memcpy(buffer, response.bytes, response.size);
	*out_size    = response.size;
	*out_overrun = (response.flags & PS2_TRANSPORT_READ_FLAG_OVERRUN) != 0u;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t transport_write(cap_id_t transport_cap, uint8_t value) {
	const struct ps2_transport_write_request request = {
		.header = {.op = PS2_TRANSPORT_OP_WRITE},
		.value  = value,
	};
	size_t response_size = 0u;
	if (transport_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = cap_call(transport_cap, &request, sizeof(request), NULL, 0u, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

static syscall_status_t transport_acquire_receive_signal(cap_id_t transport_cap, cap_id_t* out_signal_cap) {
	const struct ps2_transport_acquire_signal_request request = {
		.header = {.op = PS2_TRANSPORT_OP_ACQUIRE_RECEIVE_SIGNAL},
	};
	struct ps2_transport_acquire_signal_response response = {.capability = CAP_ID_INVALID};
	size_t                                       response_size;
	syscall_status_t                             status;

	if (transport_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	status          = cap_call(transport_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}

static bool is_mouse_id(uint8_t value) {
	return value == PS2_MOUSE_ID_STANDARD || value == PS2_MOUSE_ID_WHEEL || value == PS2_MOUSE_ID_FIVE_BUTTON;
}

/* Initialization cannot block on the notification signal because it needs a bounded response window. */
static enum ps2_receive_result receive_byte(struct ps2_device* device, size_t poll_limit, uint8_t* out_value) {
	for (size_t attempt = 0u; attempt < poll_limit; attempt++) {
		size_t           size;
		bool             overrun;
		syscall_status_t status = transport_read(device->transport_cap, out_value, 1u, &size, &overrun);
		if (status == SYSCALL_STATUS_OK) {
			if (overrun) return PS2_RECEIVE_OVERRUN;
			if (size == 1u) return PS2_RECEIVE_OK;
			return PS2_RECEIVE_FAILED;
		}
		if (status != SYSCALL_STATUS_UNAVAILABLE) return PS2_RECEIVE_FAILED;
		if (sched_yield() != SYSCALL_STATUS_OK) return PS2_RECEIVE_FAILED;
	}
	return PS2_RECEIVE_TIMEOUT;
}

static bool drain_transport(struct ps2_device* device) {
	uint8_t bytes[PS2_SERVICE_READ_SIZE];
	for (;;) {
		size_t           size;
		bool             overrun;
		syscall_status_t status = transport_read(device->transport_cap, bytes, sizeof(bytes), &size, &overrun);
		if (status == SYSCALL_STATUS_UNAVAILABLE) return true;
		if (status != SYSCALL_STATUS_OK) return false;
		(void)size;
		(void)overrun;
	}
}

static bool command_acknowledged(struct ps2_device* device, uint8_t command) {
	for (size_t attempt = 0u; attempt < PS2_COMMAND_ATTEMPTS; attempt++) {
		if (transport_write(device->transport_cap, command) != SYSCALL_STATUS_OK) return false;
		for (size_t skipped = 0u; skipped < PS2_UNEXPECTED_RESPONSE_LIMIT; skipped++) {
			uint8_t                 response;
			enum ps2_receive_result result = receive_byte(device, PS2_RESPONSE_POLL_LIMIT, &response);
			if (result != PS2_RECEIVE_OK) return false;
			if (response == PS2_ACK) return true;
			if (response == PS2_RESEND) break;
		}
	}
	return false;
}

static bool command_with_value(struct ps2_device* device, uint8_t command, uint8_t value) {
	return command_acknowledged(device, command) && command_acknowledged(device, value);
}

static bool identify(struct ps2_device* device, uint8_t* out_first, uint8_t* out_second, size_t* out_size) {
	*out_size = 0u;
	if (!command_acknowledged(device, PS2_COMMAND_IDENTIFY)) return false;
	enum ps2_receive_result result = receive_byte(device, PS2_OPTIONAL_ID_POLL_LIMIT, out_first);
	if (result == PS2_RECEIVE_TIMEOUT) return true;
	if (result != PS2_RECEIVE_OK) return false;
	*out_size = 1u;
	if (*out_first != 0xabu) return true;
	result = receive_byte(device, PS2_OPTIONAL_ID_POLL_LIMIT, out_second);
	if (result != PS2_RECEIVE_OK) return false;
	*out_size = 2u;
	return true;
}

static bool reset_and_identify(struct ps2_device* device) {
	uint8_t first;
	uint8_t second;
	size_t  id_size;

	device->kind     = PS2_DEVICE_UNKNOWN;
	device->mouse_id = PS2_MOUSE_ID_STANDARD;
	if (!drain_transport(device) || !command_acknowledged(device, PS2_COMMAND_RESET)) return false;
	if (receive_byte(device, PS2_RESPONSE_POLL_LIMIT, &first) != PS2_RECEIVE_OK || first != PS2_SELF_TEST_SUCCESS)
		return false;
	if (!command_acknowledged(device, PS2_COMMAND_DISABLE_SCANNING) || !identify(device, &first, &second, &id_size))
		return false;

	if (id_size >= 1u && is_mouse_id(first)) {
		device->kind     = PS2_DEVICE_MOUSE;
		device->mouse_id = first;
	}
	else if (id_size == 2u && first == 0xabu) {
		(void)second;
		device->kind = PS2_DEVICE_KEYBOARD;
	}
	else if (id_size == 0u) device->kind = PS2_DEVICE_KEYBOARD;
	return device->kind != PS2_DEVICE_UNKNOWN;
}

static bool read_mouse_id(struct ps2_device* device, uint8_t* out_id) {
	uint8_t ignored;
	size_t  size;
	if (!identify(device, out_id, &ignored, &size)) return false;
	return size == 1u && is_mouse_id(*out_id);
}

static bool set_sample_rate(struct ps2_device* device, uint8_t value) {
	return command_with_value(device, PS2_COMMAND_SET_SAMPLE_RATE, value);
}

static bool configure_mouse(struct ps2_device* device) {
	uint8_t id;
	if (device->mouse_id == PS2_MOUSE_ID_STANDARD) {
		if (!set_sample_rate(device, 200u) || !set_sample_rate(device, 100u) || !set_sample_rate(device, 80u) ||
		    !read_mouse_id(device, &id))
			return false;
		device->mouse_id = id;
	}
	if (device->mouse_id == PS2_MOUSE_ID_WHEEL) {
		if (!set_sample_rate(device, 200u) || !set_sample_rate(device, 200u) || !set_sample_rate(device, 80u) ||
		    !read_mouse_id(device, &id))
			return false;
		device->mouse_id = id;
	}
	return command_acknowledged(device, PS2_COMMAND_ENABLE_SCANNING);
}

static bool configure_device(struct ps2_device* device) {
	if (!reset_and_identify(device)) return false;
	if (device->kind == PS2_DEVICE_KEYBOARD)
		return command_with_value(device, PS2_COMMAND_SET_SCAN_CODE_SET, PS2_SCAN_CODE_SET_2) &&
		       command_acknowledged(device, PS2_COMMAND_ENABLE_SCANNING);
	return configure_mouse(device);
}

static bool wait_for_notification(struct ps2_device* device) {
	struct signal_message message;
	return signal_wait(device->receive_signal_cap, &message) == SYSCALL_STATUS_OK;
}

static bool serve_keyboard(struct ps2_device* device, bool* out_reinitialize) {
	struct ps2_keyboard_decoder decoder;
	uint8_t                     bytes[PS2_SERVICE_READ_SIZE];
	ps2_keyboard_decoder_init(&decoder);
	for (;;) {
		size_t           size;
		bool             overrun;
		syscall_status_t status = transport_read(device->transport_cap, bytes, sizeof(bytes), &size, &overrun);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			if (!wait_for_notification(device)) return false;
			continue;
		}
		if (status != SYSCALL_STATUS_OK) return false;
		if (overrun) {
			if (!ps2_events_reset()) return false;
			*out_reinitialize = true;
			return true;
		}
		for (size_t index = 0u; index < size; index++) {
			if (!ps2_keyboard_decoder_feed(&decoder, bytes[index])) {
				if (ps2_events_failed() || !ps2_events_reset()) return false;
				*out_reinitialize = true;
				return true;
			}
		}
	}
}

static bool serve_mouse(struct ps2_device* device, bool* out_reinitialize) {
	struct ps2_mouse_decoder decoder;
	uint8_t                  bytes[PS2_SERVICE_READ_SIZE];
	ps2_mouse_decoder_init(&decoder, device->mouse_id);
	for (;;) {
		size_t           size;
		bool             overrun;
		syscall_status_t status = transport_read(device->transport_cap, bytes, sizeof(bytes), &size, &overrun);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			if (!wait_for_notification(device)) return false;
			continue;
		}
		if (status != SYSCALL_STATUS_OK) return false;
		if (overrun) {
			if (!ps2_events_reset()) return false;
			*out_reinitialize = true;
			return true;
		}
		for (size_t index = 0u; index < size; index++) {
			if (!ps2_mouse_decoder_feed(&decoder, bytes[index])) {
				if (ps2_events_failed() || !ps2_events_reset()) return false;
				*out_reinitialize = true;
				return true;
			}
		}
	}
}

static bool serve_device(struct ps2_device* device, bool* out_reinitialize) {
	return device->kind == PS2_DEVICE_KEYBOARD ? serve_keyboard(device, out_reinitialize)
	                                           : serve_mouse(device, out_reinitialize);
}

bool ps2_driver_run(cap_id_t device_cap) {
	struct ps2_device device = {
		.transport_cap      = CAP_ID_INVALID,
		.receive_signal_cap = CAP_ID_INVALID,
	};
	bool success           = false;
	bool input_initialized = false;

	if (device_resource_acquire(device_cap,
	                            PS2_TRANSPORT_RESOURCE,
	                            sizeof(PS2_TRANSPORT_RESOURCE) - 1u,
	                            PS2_TRANSPORT_DRIVER_RIGHTS,
	                            &device.transport_cap) != SYSCALL_STATUS_OK) {
		printf("ps2: could not acquire the byte transport\n");
		goto done;
	}
	if (transport_acquire_receive_signal(device.transport_cap, &device.receive_signal_cap) != SYSCALL_STATUS_OK) {
		printf("ps2: could not acquire the receive signal\n");
		goto done;
	}
	for (;;) {
		if (!configure_device(&device)) {
			printf("ps2: device identification or configuration failed\n");
			goto done;
		}
		if ((!input_initialized && !ps2_events_initialize(device.kind, device.mouse_id)) ||
		    (input_initialized && !ps2_events_match(device.kind, device.mouse_id))) {
			printf("ps2: could not publish a stable input service\n");
			goto done;
		}
		input_initialized = true;
		if (device.kind == PS2_DEVICE_KEYBOARD) printf("ps2: keyboard online using scan-code set 2\n");
		else printf("ps2: mouse online with device ID 0x%02x\n", (unsigned)device.mouse_id);
		bool reinitialize = false;
		if (!serve_device(&device, &reinitialize)) goto done;
		if (!reinitialize) break;
		printf("ps2: transport lost synchronization; reinitializing device\n");
	}
	success = true;

done:
	if (device.receive_signal_cap != CAP_ID_INVALID) (void)cap_drop(device.receive_signal_cap);
	if (device.transport_cap != CAP_ID_INVALID) (void)cap_drop(device.transport_cap);
	return success;
}
