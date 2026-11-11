#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/process.h>

#include <protocol/device.h>
#include <protocol/input.h>
#include <protocol/usb.h>
#include <system/capability.h>
#include <system/process.h>
#include <system/time.h>
#include <runtime/device.h>

#include "input.h"

#define HID_CLASS 3u
#define HID_BOOT_SUBCLASS 1u
#define HID_KEYBOARD_PROTOCOL 1u
#define HID_MOUSE_PROTOCOL 2u

static const enum input_key_code usage_keys[0x68] = {
	[0x04] = INPUT_KEY_A,
	[0x05] = INPUT_KEY_B,
	[0x06] = INPUT_KEY_C,
	[0x07] = INPUT_KEY_D,
	[0x08] = INPUT_KEY_E,
	[0x09] = INPUT_KEY_F,
	[0x0a] = INPUT_KEY_G,
	[0x0b] = INPUT_KEY_H,
	[0x0c] = INPUT_KEY_I,
	[0x0d] = INPUT_KEY_J,
	[0x0e] = INPUT_KEY_K,
	[0x0f] = INPUT_KEY_L,
	[0x10] = INPUT_KEY_M,
	[0x11] = INPUT_KEY_N,
	[0x12] = INPUT_KEY_O,
	[0x13] = INPUT_KEY_P,
	[0x14] = INPUT_KEY_Q,
	[0x15] = INPUT_KEY_R,
	[0x16] = INPUT_KEY_S,
	[0x17] = INPUT_KEY_T,
	[0x18] = INPUT_KEY_U,
	[0x19] = INPUT_KEY_V,
	[0x1a] = INPUT_KEY_W,
	[0x1b] = INPUT_KEY_X,
	[0x1c] = INPUT_KEY_Y,
	[0x1d] = INPUT_KEY_Z,
	[0x1e] = INPUT_KEY_1,
	[0x1f] = INPUT_KEY_2,
	[0x20] = INPUT_KEY_3,
	[0x21] = INPUT_KEY_4,
	[0x22] = INPUT_KEY_5,
	[0x23] = INPUT_KEY_6,
	[0x24] = INPUT_KEY_7,
	[0x25] = INPUT_KEY_8,
	[0x26] = INPUT_KEY_9,
	[0x27] = INPUT_KEY_0,
	[0x28] = INPUT_KEY_ENTER,
	[0x29] = INPUT_KEY_ESCAPE,
	[0x2a] = INPUT_KEY_BACKSPACE,
	[0x2b] = INPUT_KEY_TAB,
	[0x2c] = INPUT_KEY_SPACE,
	[0x2d] = INPUT_KEY_MINUS,
	[0x2e] = INPUT_KEY_EQUAL,
	[0x2f] = INPUT_KEY_LEFT_BRACKET,
	[0x30] = INPUT_KEY_RIGHT_BRACKET,
	[0x31] = INPUT_KEY_BACKSLASH,
	[0x32] = INPUT_KEY_NON_US_BACKSLASH,
	[0x33] = INPUT_KEY_SEMICOLON,
	[0x34] = INPUT_KEY_APOSTROPHE,
	[0x35] = INPUT_KEY_GRAVE,
	[0x36] = INPUT_KEY_COMMA,
	[0x37] = INPUT_KEY_PERIOD,
	[0x38] = INPUT_KEY_SLASH,
	[0x39] = INPUT_KEY_CAPS_LOCK,
	[0x3a] = INPUT_KEY_F1,
	[0x3b] = INPUT_KEY_F2,
	[0x3c] = INPUT_KEY_F3,
	[0x3d] = INPUT_KEY_F4,
	[0x3e] = INPUT_KEY_F5,
	[0x3f] = INPUT_KEY_F6,
	[0x40] = INPUT_KEY_F7,
	[0x41] = INPUT_KEY_F8,
	[0x42] = INPUT_KEY_F9,
	[0x43] = INPUT_KEY_F10,
	[0x44] = INPUT_KEY_F11,
	[0x45] = INPUT_KEY_F12,
	[0x46] = INPUT_KEY_PRINT_SCREEN,
	[0x47] = INPUT_KEY_SCROLL_LOCK,
	[0x48] = INPUT_KEY_PAUSE,
	[0x49] = INPUT_KEY_INSERT,
	[0x4a] = INPUT_KEY_HOME,
	[0x4b] = INPUT_KEY_PAGE_UP,
	[0x4c] = INPUT_KEY_DELETE,
	[0x4d] = INPUT_KEY_END,
	[0x4e] = INPUT_KEY_PAGE_DOWN,
	[0x4f] = INPUT_KEY_RIGHT,
	[0x50] = INPUT_KEY_LEFT,
	[0x51] = INPUT_KEY_DOWN,
	[0x52] = INPUT_KEY_UP,
	[0x53] = INPUT_KEY_NUM_LOCK,
	[0x54] = INPUT_KEY_KEYPAD_DIVIDE,
	[0x55] = INPUT_KEY_KEYPAD_MULTIPLY,
	[0x56] = INPUT_KEY_KEYPAD_SUBTRACT,
	[0x57] = INPUT_KEY_KEYPAD_ADD,
	[0x58] = INPUT_KEY_KEYPAD_ENTER,
	[0x59] = INPUT_KEY_KEYPAD_1,
	[0x5a] = INPUT_KEY_KEYPAD_2,
	[0x5b] = INPUT_KEY_KEYPAD_3,
	[0x5c] = INPUT_KEY_KEYPAD_4,
	[0x5d] = INPUT_KEY_KEYPAD_5,
	[0x5e] = INPUT_KEY_KEYPAD_6,
	[0x5f] = INPUT_KEY_KEYPAD_7,
	[0x60] = INPUT_KEY_KEYPAD_8,
	[0x61] = INPUT_KEY_KEYPAD_9,
	[0x62] = INPUT_KEY_KEYPAD_0,
	[0x63] = INPUT_KEY_KEYPAD_DECIMAL,
	[0x64] = INPUT_KEY_NON_US_BACKSLASH,
	[0x65] = INPUT_KEY_MENU,
};
static const enum input_key_code modifier_keys[8] = {
	INPUT_KEY_LEFT_CONTROL,
	INPUT_KEY_LEFT_SHIFT,
	INPUT_KEY_LEFT_ALT,
	INPUT_KEY_LEFT_META,
	INPUT_KEY_RIGHT_CONTROL,
	INPUT_KEY_RIGHT_SHIFT,
	INPUT_KEY_RIGHT_ALT,
	INPUT_KEY_RIGHT_META,
};

static syscall_status_t control(cap_id_t transport, uint8_t request, uint16_t value, uint8_t interface_number) {
	struct usb_transport_control_request command = {
		.header = {.op = USB_TRANSPORT_OP_CONTROL},
		.request_type = 0x21u,
		.request = request,
		.value = value,
		.index = interface_number,
		.length = 0u,
	};
	struct usb_transport_transfer_response response;
	size_t size = 0u;
	syscall_status_t status = cap_call(transport, &command, sizeof(command), &response, sizeof(response), &size);
	return status == SYSCALL_STATUS_OK && size == sizeof(response) && response.transferred == 0u
				   ? SYSCALL_STATUS_OK
				   : SYSCALL_STATUS_FAILED;
}

enum report_read_result {
	REPORT_READ_ERROR,
	REPORT_READ_PENDING,
	REPORT_READ_READY,
};

static enum report_read_result read_report(cap_id_t transport, uint8_t endpoint, uint8_t* report, size_t length,
		uint8_t* response, size_t response_capacity, size_t* received) {
	struct usb_transport_transfer_request command = {
		.header = {.op = USB_TRANSPORT_OP_TRANSFER},
		.endpoint = endpoint,
		.length = (uint32_t)length,
	};
	size_t size = 0u;
	syscall_status_t status = cap_call(transport, &command, sizeof(command), response, response_capacity, &size);
	if (status == SYSCALL_STATUS_UNAVAILABLE) return REPORT_READ_PENDING;
	if (status != SYSCALL_STATUS_OK || size < sizeof(struct usb_transport_transfer_response)) return REPORT_READ_ERROR;
	struct usb_transport_transfer_response header;
	memcpy(&header, response, sizeof(header));
	if (header.transferred > length || size != sizeof(header) + header.transferred) return REPORT_READ_ERROR;
	memcpy(report, response + sizeof(header), header.transferred);
	*received = header.transferred;
	return REPORT_READ_READY;
}

static bool keyboard_report(struct hid_input_server* server, bool* state, const uint8_t* report) {
	bool next[INPUT_KEY_KEYPAD_DECIMAL + 1u] = {0};
	for (unsigned i = 0u; i < 8u; i++) next[modifier_keys[i]] = (report[0] & (1u << i)) != 0u;
	for (unsigned i = 2u; i < 8u; i++) {
		uint8_t usage = report[i];
		if (usage >= 1u && usage <= 3u) return true; /* Rollover/error report. */
		if (usage < sizeof(usage_keys) / sizeof(usage_keys[0]) && usage_keys[usage] != INPUT_KEY_INVALID)
			next[usage_keys[usage]] = true;
	}
	struct hid_input_value values[INPUT_KEY_KEYPAD_DECIMAL];
	size_t count = 0u;
	for (unsigned key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++) {
		if (state[key] != next[key]) values[count++] = (struct hid_input_value){.control_id = key, .value = next[key]};
	}
	if (count != 0u && hid_input_server_publish(server, values, count) != SYSCALL_STATUS_OK) return false;
	memcpy(state, next, sizeof(next));
	return true;
}

static bool mouse_report(struct hid_input_server* server, uint8_t* old_buttons, const uint8_t* report) {
	struct hid_input_value values[5];
	size_t count = 0u;
	int8_t dx = (int8_t)report[1], dy = (int8_t)report[2];
	if (dx) values[count++] = (struct hid_input_value){.control_id = 1u, .value = dx};
	if (dy) values[count++] = (struct hid_input_value){.control_id = 2u, .value = dy};
	for (unsigned i = 0u; i < 3u; i++) {
		if (((*old_buttons ^ report[0]) & (1u << i)) != 0u)
			values[count++] = (struct hid_input_value){.control_id = 3u + i, .value = (report[0] >> i) & 1u};
	}
	if (count && hid_input_server_publish(server, values, count) != SYSCALL_STATUS_OK) return false;
	*old_buttons = report[0];
	return true;
}

static enum device_driver_result run(cap_id_t device_cap) {
	cap_id_t transport = CAP_ID_INVALID;
	if (device_resource_acquire(device_cap,
				USB_TRANSPORT_RESOURCE,
				sizeof(USB_TRANSPORT_RESOURCE) - 1u,
				USB_TRANSPORT_RIGHTS,
				&transport) != SYSCALL_STATUS_OK)
		return DEVICE_DRIVER_ERROR;
	enum device_driver_result result = DEVICE_DRIVER_ERROR;
	uint8_t* report = NULL;
	uint8_t* response = NULL;
	struct usb_transport_info info;
	struct usb_transport_header query = {.op = USB_TRANSPORT_OP_INFO};
	size_t size = 0u;
	if (cap_call(transport, &query, sizeof(query), &info, sizeof(info), &size) != SYSCALL_STATUS_OK ||
			size != sizeof(info))
		goto done;
	if (info.class_code != HID_CLASS || info.subclass != HID_BOOT_SUBCLASS ||
			(info.programming_interface != HID_KEYBOARD_PROTOCOL && info.programming_interface != HID_MOUSE_PROTOCOL)) {
		result = DEVICE_DRIVER_UNSUPPORTED;
		goto done;
	}
	uint8_t endpoint = 0u;
	size_t report_length = info.programming_interface == HID_KEYBOARD_PROTOCOL ? 8u : 3u;
	size_t transfer_length = 0u;
	for (unsigned i = 0u; i < info.endpoint_count; i++) {
		size_t packet_size = info.endpoints[i].max_packet_size & 0x7ffu;
		if ((info.endpoints[i].address & 0x80u) != 0u && (info.endpoints[i].attributes & 3u) == 3u &&
				packet_size >= report_length && packet_size <= USB_TRANSPORT_MAX_TRANSFER) {
			endpoint = info.endpoints[i].address;
			transfer_length = packet_size;
			break;
		}
	}
	if (!endpoint) {
		result = DEVICE_DRIVER_UNSUPPORTED;
		goto done;
	}
	report = malloc(transfer_length);
	response = malloc(sizeof(struct usb_transport_transfer_response) + transfer_length);
	if (report == NULL || response == NULL) goto done;
	if (control(transport, 0x0bu, 0u, info.interface_number) != SYSCALL_STATUS_OK) goto done;
	if (info.programming_interface == HID_KEYBOARD_PROTOCOL) (void)control(transport, 0x0au, 0u, info.interface_number);
	struct input_control_descriptor descriptors[INPUT_KEY_KEYPAD_DECIMAL];
	size_t count = 0u;
	if (info.programming_interface == HID_KEYBOARD_PROTOCOL) {
		for (unsigned key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++)
			descriptors[count++] = (struct input_control_descriptor){
				.id = key, .kind = INPUT_CONTROL_KIND_KEY, .code = key, .logical_maximum = 1
			};
	}
	else {
		descriptors[count++] = (struct input_control_descriptor){
			.id = 1u, .kind = INPUT_CONTROL_KIND_RELATIVE_AXIS, .code = INPUT_AXIS_X
		};
		descriptors[count++] = (struct input_control_descriptor){
			.id = 2u, .kind = INPUT_CONTROL_KIND_RELATIVE_AXIS, .code = INPUT_AXIS_Y
		};
		for (unsigned i = 0u; i < 3u; i++)
			descriptors[count++] = (struct input_control_descriptor){
				.id = 3u + i, .kind = INPUT_CONTROL_KIND_BUTTON, .code = INPUT_BUTTON_PRIMARY + i, .logical_maximum = 1
			};
	}
	struct self_info self = {0};
	if (process_self_info(&self) != SYSCALL_STATUS_OK) goto done;
	char service[32];
	int n = snprintf(service, sizeof(service), "hid_%llu", (unsigned long long)self.pid);
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (n <= 0 || (size_t)n >= sizeof(service)) goto done;
	struct hid_input_server* server = NULL;
	if (hid_input_server_create(service,
				info.programming_interface == HID_KEYBOARD_PROTOCOL ? INPUT_DEVICE_CLASS_KEYBOARD
				                                                    : INPUT_DEVICE_CLASS_POINTER,
				descriptors,
				count,
				NULL,
				0u,
				CAP_ID_INVALID,
				&server) != SYSCALL_STATUS_OK)
		goto done;
	printf("hid: USB boot %s online\n", info.programming_interface == HID_KEYBOARD_PROTOCOL ? "keyboard" : "mouse");
	bool keys[INPUT_KEY_KEYPAD_DECIMAL + 1u] = {0};
	uint8_t buttons = 0u;
	for (;;) {
		size_t received = 0u;
		enum report_read_result read = read_report(transport,
				endpoint,
				report,
				transfer_length,
				response,
				sizeof(struct usb_transport_transfer_response) + transfer_length,
				&received);
		if (read == REPORT_READ_ERROR) break;
		if (read == REPORT_READ_READY && received >= report_length) {
			if (info.programming_interface == HID_KEYBOARD_PROTOCOL) {
				if (!keyboard_report(server, keys, report)) break;
			}
			else if (!mouse_report(server, &buttons, report)) break;
		}
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) break;
	}
done:
	free(response);
	free(report);
	(void)cap_drop(transport);
	return result;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argc;
	(void)argv;
	if (capc != DEVICE_DRIVER_CAPABILITY_COUNT || capv == NULL ||
			capv[DEVICE_DRIVER_CAPABILITY_DEVICE] == CAP_ID_INVALID)
		return 1;
	enum device_driver_result result = run(capv[DEVICE_DRIVER_CAPABILITY_DEVICE]);
	(void)cap_drop(capv[DEVICE_DRIVER_CAPABILITY_DEVICE]);
	return result == DEVICE_DRIVER_UNSUPPORTED ? DEVICE_DRIVER_EXIT_UNSUPPORTED : 1;
}
