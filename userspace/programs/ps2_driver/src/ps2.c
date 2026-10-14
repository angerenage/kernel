#include "ps2.h"

#include <protocol/device.h>
#include <protocol/ps2.h>
#include <runtime/device.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/process.h>
#include <system/signal.h>
#include <system/thread.h>
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
#define PS2_COMMAND_SET_LEDS 0xedu
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
	cap_id_t             activity_cap;
	cap_id_t             transport_waiter_cap;
	enum ps2_device_kind kind;
	uint8_t              mouse_id;
	uint8_t              keyboard_leds;
	bool                 transport_failed;
};

/* Copied onto the transport waiter's initial stack. */
struct ps2_transport_waiter_argument {
	struct ps2_device* device;
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

static void transport_waiter(void* opaque) {
	const struct ps2_transport_waiter_argument* argument = opaque;
	struct ps2_device*                          device   = argument->device;
	for (;;) {
		struct signal_message message;
		syscall_status_t      status = signal_wait(device->receive_signal_cap, &message);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		if (status != SYSCALL_STATUS_OK) __atomic_store_n(&device->transport_failed, true, __ATOMIC_RELEASE);
		struct signal_send_response response;
		if (signal_send(device->activity_cap, 1u, 0u, 0u, 0u, SIGNAL_SEND_FLAG_COALESCE, &response) !=
		    SYSCALL_STATUS_OK)
			thread_exit(1u);
		if (status != SYSCALL_STATUS_OK) thread_exit(1u);
	}
}

static bool start_transport_waiter(struct ps2_device* device) {
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID};
	if (signal_create(&device->activity_cap) != SYSCALL_STATUS_OK) return false;
	syscall_status_t status = process_self_info(&self);
	if (status == SYSCALL_STATUS_OK) {
		const struct ps2_transport_waiter_argument argument = {.device = device};
		status                                              = process_spawn_thread(self.self_cap,
		                                                                           (uintptr_t)transport_waiter,
		                                                                           &argument,
		                                                                           sizeof(argument),
		                                                                           "PS/2 transport waiter",
		                                                                           sizeof("PS/2 transport waiter"),
		                                                                           &device->transport_waiter_cap);
	}
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	return status == SYSCALL_STATUS_OK;
}

static bool stop_transport_waiter(struct ps2_device* device) {
	bool success = true;
	if (device->transport_waiter_cap != CAP_ID_INVALID) {
		(void)thread_cancel(device->transport_waiter_cap);
		if (thread_join(device->transport_waiter_cap, NULL) != SYSCALL_STATUS_OK) success = false;
		device->transport_waiter_cap = CAP_ID_INVALID;
	}
	if (device->activity_cap != CAP_ID_INVALID) {
		if (signal_destroy(device->activity_cap) != SYSCALL_STATUS_OK) success = false;
		device->activity_cap = CAP_ID_INVALID;
	}
	return success;
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
		       command_with_value(device, PS2_COMMAND_SET_LEDS, device->keyboard_leds) &&
		       command_acknowledged(device, PS2_COMMAND_ENABLE_SCANNING);
	return configure_mouse(device);
}

static bool wait_for_activity(struct ps2_device* device) {
	struct signal_message message;
	for (;;) {
		syscall_status_t status = signal_wait(device->activity_cap, &message);
		if (status == SYSCALL_STATUS_INTERRUPTED) continue;
		return status == SYSCALL_STATUS_OK && !__atomic_load_n(&device->transport_failed, __ATOMIC_ACQUIRE);
	}
}

static bool keyboard_command_acknowledged(struct ps2_device* device, struct ps2_keyboard_decoder* decoder,
                                          uint8_t command) {
	for (size_t attempt = 0u; attempt < PS2_COMMAND_ATTEMPTS; attempt++) {
		if (transport_write(device->transport_cap, command) != SYSCALL_STATUS_OK) return false;
		for (size_t skipped = 0u; skipped < PS2_UNEXPECTED_RESPONSE_LIMIT; skipped++) {
			uint8_t                 response;
			enum ps2_receive_result result = receive_byte(device, PS2_RESPONSE_POLL_LIMIT, &response);
			if (result != PS2_RECEIVE_OK) return false;
			if (response == PS2_ACK) return true;
			if (response == PS2_RESEND) break;
			if (!ps2_keyboard_decoder_feed(decoder, response) || ps2_events_failed()) return false;
		}
	}
	return false;
}

static bool keyboard_set_leds(struct ps2_device* device, struct ps2_keyboard_decoder* decoder, uint8_t mask) {
	return keyboard_command_acknowledged(device, decoder, PS2_COMMAND_SET_LEDS) &&
	       keyboard_command_acknowledged(device, decoder, mask);
}

static bool keyboard_output_mask(struct ps2_device* device, const struct input_output_value* values, size_t count,
                                 uint8_t* out_mask) {
	uint8_t mask = device->keyboard_leds;
	for (size_t i = 0u; i < count; i++) {
		uint8_t bit;
		switch (values[i].output_id) {
		case PS2_KEYBOARD_OUTPUT_NUM_LOCK:
			bit = 1u << 1u;
			break;
		case PS2_KEYBOARD_OUTPUT_CAPS_LOCK:
			bit = 1u << 2u;
			break;
		case PS2_KEYBOARD_OUTPUT_SCROLL_LOCK:
			bit = 1u << 0u;
			break;
		default:
			return false;
		}
		if (values[i].value != 0) mask |= bit;
		else mask &= (uint8_t)~bit;
	}
	*out_mask = mask;
	return true;
}

static bool serve_keyboard(struct ps2_device* device, bool* out_reinitialize) {
	struct ps2_keyboard_decoder decoder;
	uint8_t                     bytes[PS2_SERVICE_READ_SIZE];
	ps2_keyboard_decoder_init(&decoder);
	for (;;) {
		struct input_output_value outputs[3];
		size_t                    output_count;
		if (ps2_outputs_take(outputs, sizeof(outputs) / sizeof(outputs[0]), &output_count)) {
			uint8_t mask;
			bool    applied =
				keyboard_output_mask(device, outputs, output_count, &mask) && keyboard_set_leds(device, &decoder, mask);
			if (applied) device->keyboard_leds = mask;
			if (!ps2_outputs_complete(applied ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED)) return false;
			if (!applied) {
				if (ps2_events_failed() || !ps2_events_reset()) return false;
				*out_reinitialize = true;
				return true;
			}
			continue;
		}

		size_t           size;
		bool             overrun;
		syscall_status_t status = transport_read(device->transport_cap, bytes, sizeof(bytes), &size, &overrun);
		if (status == SYSCALL_STATUS_UNAVAILABLE) {
			if (!wait_for_activity(device)) return false;
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
			if (!wait_for_activity(device)) return false;
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
		.transport_cap        = CAP_ID_INVALID,
		.receive_signal_cap   = CAP_ID_INVALID,
		.activity_cap         = CAP_ID_INVALID,
		.transport_waiter_cap = CAP_ID_INVALID,
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
	if (!start_transport_waiter(&device)) {
		printf("ps2: could not start the transport waiter\n");
		goto done;
	}
	for (;;) {
		if (!configure_device(&device)) {
			printf("ps2: device identification or configuration failed\n");
			goto done;
		}
		if ((!input_initialized && !ps2_events_initialize(device.kind, device.mouse_id, device.activity_cap)) ||
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
	if (!stop_transport_waiter(&device)) success = false;
	if (device.receive_signal_cap != CAP_ID_INVALID) (void)cap_drop(device.receive_signal_cap);
	if (device.transport_cap != CAP_ID_INVALID) (void)cap_drop(device.transport_cap);
	return success;
}
