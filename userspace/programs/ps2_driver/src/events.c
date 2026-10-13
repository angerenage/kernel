#include "events.h"

#include <protocol/input.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/process.h>

#include "input.h"

#define PS2_MOUSE_ID_STANDARD 0x00u
#define PS2_MOUSE_ID_WHEEL 0x03u
#define PS2_MOUSE_ID_FIVE_BUTTON 0x04u

enum ps2_mouse_control_id {
	PS2_MOUSE_CONTROL_X = 1u,
	PS2_MOUSE_CONTROL_Y,
	PS2_MOUSE_CONTROL_WHEEL,
	PS2_MOUSE_CONTROL_PRIMARY,
	PS2_MOUSE_CONTROL_SECONDARY,
	PS2_MOUSE_CONTROL_MIDDLE,
	PS2_MOUSE_CONTROL_BACK,
	PS2_MOUSE_CONTROL_FORWARD,
};

struct scan_code_mapping {
	uint8_t             scan_code;
	enum input_key_code key;
};

/* Standard MF2 Scan Set 2 positions excluding the E0-prefixed extension map. */
static const struct scan_code_mapping base_keys[] = {
	{0x01u,               INPUT_KEY_F9},
	{0x03u,               INPUT_KEY_F5},
	{0x04u,               INPUT_KEY_F3},
	{0x05u,               INPUT_KEY_F1},
	{0x06u,               INPUT_KEY_F2},
	{0x07u,              INPUT_KEY_F12},
	{0x09u,              INPUT_KEY_F10},
	{0x0au,               INPUT_KEY_F8},
	{0x0bu,               INPUT_KEY_F6},
	{0x0cu,               INPUT_KEY_F4},
	{0x0du,              INPUT_KEY_TAB},
	{0x0eu,            INPUT_KEY_GRAVE},
	{0x11u,         INPUT_KEY_LEFT_ALT},
	{0x12u,       INPUT_KEY_LEFT_SHIFT},
	{0x14u,     INPUT_KEY_LEFT_CONTROL},
	{0x15u,                INPUT_KEY_Q},
	{0x16u,                INPUT_KEY_1},
	{0x1au,                INPUT_KEY_Z},
	{0x1bu,                INPUT_KEY_S},
	{0x1cu,                INPUT_KEY_A},
	{0x1du,                INPUT_KEY_W},
	{0x1eu,                INPUT_KEY_2},
	{0x21u,                INPUT_KEY_C},
	{0x22u,                INPUT_KEY_X},
	{0x23u,                INPUT_KEY_D},
	{0x24u,                INPUT_KEY_E},
	{0x25u,                INPUT_KEY_4},
	{0x26u,                INPUT_KEY_3},
	{0x29u,            INPUT_KEY_SPACE},
	{0x2au,                INPUT_KEY_V},
	{0x2bu,                INPUT_KEY_F},
	{0x2cu,                INPUT_KEY_T},
	{0x2du,                INPUT_KEY_R},
	{0x2eu,                INPUT_KEY_5},
	{0x31u,                INPUT_KEY_N},
	{0x32u,                INPUT_KEY_B},
	{0x33u,                INPUT_KEY_H},
	{0x34u,                INPUT_KEY_G},
	{0x35u,                INPUT_KEY_Y},
	{0x36u,                INPUT_KEY_6},
	{0x3au,                INPUT_KEY_M},
	{0x3bu,                INPUT_KEY_J},
	{0x3cu,                INPUT_KEY_U},
	{0x3du,                INPUT_KEY_7},
	{0x3eu,                INPUT_KEY_8},
	{0x41u,            INPUT_KEY_COMMA},
	{0x42u,                INPUT_KEY_K},
	{0x43u,                INPUT_KEY_I},
	{0x44u,                INPUT_KEY_O},
	{0x45u,                INPUT_KEY_0},
	{0x46u,                INPUT_KEY_9},
	{0x49u,           INPUT_KEY_PERIOD},
	{0x4au,            INPUT_KEY_SLASH},
	{0x4bu,                INPUT_KEY_L},
	{0x4cu,        INPUT_KEY_SEMICOLON},
	{0x4du,                INPUT_KEY_P},
	{0x4eu,            INPUT_KEY_MINUS},
	{0x52u,       INPUT_KEY_APOSTROPHE},
	{0x54u,     INPUT_KEY_LEFT_BRACKET},
	{0x55u,            INPUT_KEY_EQUAL},
	{0x58u,        INPUT_KEY_CAPS_LOCK},
	{0x59u,      INPUT_KEY_RIGHT_SHIFT},
	{0x5au,            INPUT_KEY_ENTER},
	{0x5bu,    INPUT_KEY_RIGHT_BRACKET},
	{0x5du,        INPUT_KEY_BACKSLASH},
	{0x61u, INPUT_KEY_NON_US_BACKSLASH},
	{0x66u,        INPUT_KEY_BACKSPACE},
	{0x69u,         INPUT_KEY_KEYPAD_1},
	{0x6bu,         INPUT_KEY_KEYPAD_4},
	{0x6cu,         INPUT_KEY_KEYPAD_7},
	{0x70u,         INPUT_KEY_KEYPAD_0},
	{0x71u,   INPUT_KEY_KEYPAD_DECIMAL},
	{0x72u,         INPUT_KEY_KEYPAD_2},
	{0x73u,         INPUT_KEY_KEYPAD_5},
	{0x74u,         INPUT_KEY_KEYPAD_6},
	{0x75u,         INPUT_KEY_KEYPAD_8},
	{0x76u,           INPUT_KEY_ESCAPE},
	{0x77u,         INPUT_KEY_NUM_LOCK},
	{0x78u,              INPUT_KEY_F11},
	{0x79u,       INPUT_KEY_KEYPAD_ADD},
	{0x7au,         INPUT_KEY_KEYPAD_3},
	{0x7bu,  INPUT_KEY_KEYPAD_SUBTRACT},
	{0x7cu,  INPUT_KEY_KEYPAD_MULTIPLY},
	{0x7du,         INPUT_KEY_KEYPAD_9},
	{0x7eu,      INPUT_KEY_SCROLL_LOCK},
	{0x83u,               INPUT_KEY_F7},
};

/* Standard E0-prefixed MF2 positions. */
static const struct scan_code_mapping extended_keys[] = {
	{0x11u,     INPUT_KEY_RIGHT_ALT},
	{0x14u, INPUT_KEY_RIGHT_CONTROL},
	{0x1fu,     INPUT_KEY_LEFT_META},
	{0x27u,    INPUT_KEY_RIGHT_META},
	{0x2fu,          INPUT_KEY_MENU},
	{0x4au, INPUT_KEY_KEYPAD_DIVIDE},
	{0x5au,  INPUT_KEY_KEYPAD_ENTER},
	{0x69u,           INPUT_KEY_END},
	{0x6bu,          INPUT_KEY_LEFT},
	{0x6cu,          INPUT_KEY_HOME},
	{0x70u,        INPUT_KEY_INSERT},
	{0x71u,        INPUT_KEY_DELETE},
	{0x72u,          INPUT_KEY_DOWN},
	{0x74u,         INPUT_KEY_RIGHT},
	{0x75u,            INPUT_KEY_UP},
	{0x7au,     INPUT_KEY_PAGE_DOWN},
	{0x7cu,  INPUT_KEY_PRINT_SCREEN},
	{0x7du,       INPUT_KEY_PAGE_UP},
};

static struct ps2_input_server* server;
static enum ps2_device_kind     advertised_kind;
static uint8_t                  advertised_mouse_id;
static bool                     key_state[INPUT_KEY_KEYPAD_DECIMAL + 1u];
static uint8_t                  mouse_buttons;
static bool                     publication_failed;

static enum input_key_code map_key(uint8_t scan_code, bool extended) {
	const struct scan_code_mapping* mappings = extended ? extended_keys : base_keys;
	size_t                          count =
		extended ? sizeof(extended_keys) / sizeof(extended_keys[0]) : sizeof(base_keys) / sizeof(base_keys[0]);
	for (size_t i = 0u; i < count; i++) {
		if (mappings[i].scan_code == scan_code) return mappings[i].key;
	}
	return INPUT_KEY_INVALID;
}

static bool publish(const struct ps2_input_value* values, size_t count) {
	if (ps2_input_server_publish(server, values, count) == SYSCALL_STATUS_OK) return true;
	publication_failed = true;
	return false;
}

static bool create_keyboard_provider(const char* service) {
	struct input_control_descriptor descriptors[INPUT_KEY_KEYPAD_DECIMAL];
	for (uint32_t key = INPUT_KEY_ESCAPE; key <= INPUT_KEY_KEYPAD_DECIMAL; key++) {
		descriptors[key - 1u] = (struct input_control_descriptor){
			.id              = key,
			.kind            = INPUT_CONTROL_KIND_KEY,
			.code            = key,
			.logical_minimum = 0,
			.logical_maximum = 1,
		};
	}
	return ps2_input_server_create(service,
	                               INPUT_DEVICE_CLASS_KEYBOARD,
	                               descriptors,
	                               sizeof(descriptors) / sizeof(descriptors[0]),
	                               &server) == SYSCALL_STATUS_OK;
}

static void set_relative_descriptor(struct input_control_descriptor* descriptor, uint32_t id,
                                    enum input_axis_code axis) {
	*descriptor = (struct input_control_descriptor){
		.id   = id,
		.kind = INPUT_CONTROL_KIND_RELATIVE_AXIS,
		.code = axis,
	};
}

static void set_button_descriptor(struct input_control_descriptor* descriptor, uint32_t id,
                                  enum input_button_code button) {
	*descriptor = (struct input_control_descriptor){
		.id              = id,
		.kind            = INPUT_CONTROL_KIND_BUTTON,
		.code            = button,
		.logical_minimum = 0,
		.logical_maximum = 1,
	};
}

static bool create_mouse_provider(const char* service, uint8_t mouse_id) {
	struct input_control_descriptor descriptors[8];
	size_t                          count = 0u;
	set_relative_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_X, INPUT_AXIS_X);
	set_relative_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_Y, INPUT_AXIS_Y);
	if (mouse_id == PS2_MOUSE_ID_WHEEL || mouse_id == PS2_MOUSE_ID_FIVE_BUTTON)
		set_relative_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_WHEEL, INPUT_AXIS_VERTICAL_SCROLL);
	set_button_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_PRIMARY, INPUT_BUTTON_PRIMARY);
	set_button_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_SECONDARY, INPUT_BUTTON_SECONDARY);
	set_button_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_MIDDLE, INPUT_BUTTON_MIDDLE);
	if (mouse_id == PS2_MOUSE_ID_FIVE_BUTTON) {
		set_button_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_BACK, INPUT_BUTTON_BACK);
		set_button_descriptor(&descriptors[count++], PS2_MOUSE_CONTROL_FORWARD, INPUT_BUTTON_FORWARD);
	}
	return ps2_input_server_create(service, INPUT_DEVICE_CLASS_POINTER, descriptors, count, &server) ==
	       SYSCALL_STATUS_OK;
}

bool ps2_events_initialize(enum ps2_device_kind kind, uint8_t mouse_id) {
	struct self_info self;
	char             service[32];
	if (server != NULL || (kind != PS2_DEVICE_KEYBOARD && kind != PS2_DEVICE_MOUSE) ||
	    process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	int service_size = sprintf(service, "ps2_%llu", (unsigned long long)self.pid);
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (service_size <= 0 || (size_t)service_size >= sizeof(service)) return false;
	bool created =
		kind == PS2_DEVICE_KEYBOARD ? create_keyboard_provider(service) : create_mouse_provider(service, mouse_id);
	if (!created) return false;
	advertised_kind     = kind;
	advertised_mouse_id = mouse_id;
	return true;
}

bool ps2_events_match(enum ps2_device_kind kind, uint8_t mouse_id) {
	return server != NULL && advertised_kind == kind && (kind != PS2_DEVICE_MOUSE || advertised_mouse_id == mouse_id);
}

bool ps2_event_key(uint8_t scan_code, bool extended, bool pressed) {
	enum input_key_code key = map_key(scan_code, extended);
	if (key == INPUT_KEY_INVALID || key_state[key] == pressed) return true;
	const struct ps2_input_value value = {.control_id = key, .value = pressed ? 1 : 0};
	if (!publish(&value, 1u)) return false;
	key_state[key] = pressed;
	return true;
}

bool ps2_event_pause(void) {
	const struct ps2_input_value pressed  = {.control_id = INPUT_KEY_PAUSE, .value = 1};
	const struct ps2_input_value released = {.control_id = INPUT_KEY_PAUSE, .value = 0};
	return publish(&pressed, 1u) && publish(&released, 1u);
}

static void append_mouse_button(struct ps2_input_value* values, size_t* count, uint8_t old_buttons, uint8_t new_buttons,
                                uint8_t mask, uint32_t control_id) {
	if (((old_buttons ^ new_buttons) & mask) == 0u) return;
	values[(*count)++] = (struct ps2_input_value){
		.control_id = control_id,
		.value      = (new_buttons & mask) != 0u ? 1 : 0,
	};
}

bool ps2_event_mouse(int16_t delta_x, int16_t delta_y, int8_t wheel, uint8_t buttons) {
	struct ps2_input_value values[8];
	size_t                 count = 0u;
	if (delta_x != 0) values[count++] = (struct ps2_input_value){PS2_MOUSE_CONTROL_X, delta_x};
	if (delta_y != 0) values[count++] = (struct ps2_input_value){PS2_MOUSE_CONTROL_Y, delta_y};
	if (wheel != 0 && advertised_mouse_id != PS2_MOUSE_ID_STANDARD)
		values[count++] = (struct ps2_input_value){PS2_MOUSE_CONTROL_WHEEL, wheel};
	append_mouse_button(values, &count, mouse_buttons, buttons, 1u << 0u, PS2_MOUSE_CONTROL_PRIMARY);
	append_mouse_button(values, &count, mouse_buttons, buttons, 1u << 1u, PS2_MOUSE_CONTROL_SECONDARY);
	append_mouse_button(values, &count, mouse_buttons, buttons, 1u << 2u, PS2_MOUSE_CONTROL_MIDDLE);
	if (advertised_mouse_id == PS2_MOUSE_ID_FIVE_BUTTON) {
		append_mouse_button(values, &count, mouse_buttons, buttons, 1u << 3u, PS2_MOUSE_CONTROL_BACK);
		append_mouse_button(values, &count, mouse_buttons, buttons, 1u << 4u, PS2_MOUSE_CONTROL_FORWARD);
	}
	if (count != 0u && !publish(values, count)) return false;
	mouse_buttons = buttons;
	return true;
}

bool ps2_events_reset(void) {
	memset(key_state, 0, sizeof(key_state));
	mouse_buttons = 0u;
	if (ps2_input_server_reset(server) == SYSCALL_STATUS_OK) return true;
	publication_failed = true;
	return false;
}

bool ps2_events_failed(void) {
	return publication_failed;
}
