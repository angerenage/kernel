#pragma once

/*
 * Unified input-device capability protocol.
 *
 * Logical input producers expose immutable control descriptors, a retained
 * framed event stream, synchronous output controls, and a coalesced signal
 * for announcing new events. Consumers keep independent sequence cursors, so
 * reading does not consume events for other clients.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <stdint.h>

/* Registry identity shared by every logical input-device service. */
#define INPUT_NAMESPACE "devices"
#define INPUT_PROTOCOL_NAME "input"

/* Rights granted to input-service clients and their notification signals. */
#define INPUT_SERVICE_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))
#define INPUT_EVENT_SIGNAL_RIGHTS ((cap_rights_t)CAP_WAIT)

/* Broad logical function exposed by one input-service instance. */
enum input_device_class {
	INPUT_DEVICE_CLASS_INVALID = 0,
	INPUT_DEVICE_CLASS_KEYBOARD,
	INPUT_DEVICE_CLASS_POINTER,
	INPUT_DEVICE_CLASS_TOUCH,
	INPUT_DEVICE_CLASS_GAME_CONTROLLER,
	INPUT_DEVICE_CLASS_OTHER,
};

/* Value semantics attached to one immutable input control. */
enum input_control_kind {
	INPUT_CONTROL_KIND_INVALID = 0,
	INPUT_CONTROL_KIND_KEY,
	INPUT_CONTROL_KIND_BUTTON,
	INPUT_CONTROL_KIND_RELATIVE_AXIS,
	INPUT_CONTROL_KIND_ABSOLUTE_AXIS,
};

/*
 * Layout-independent physical keyboard positions. Conventional US legends
 * make the names readable; they do not prescribe text or a keyboard layout.
 */
enum input_key_code {
	INPUT_KEY_INVALID = 0,
	INPUT_KEY_ESCAPE,
	INPUT_KEY_F1,
	INPUT_KEY_F2,
	INPUT_KEY_F3,
	INPUT_KEY_F4,
	INPUT_KEY_F5,
	INPUT_KEY_F6,
	INPUT_KEY_F7,
	INPUT_KEY_F8,
	INPUT_KEY_F9,
	INPUT_KEY_F10,
	INPUT_KEY_F11,
	INPUT_KEY_F12,
	INPUT_KEY_PRINT_SCREEN,
	INPUT_KEY_SCROLL_LOCK,
	INPUT_KEY_PAUSE,
	INPUT_KEY_GRAVE,
	INPUT_KEY_1,
	INPUT_KEY_2,
	INPUT_KEY_3,
	INPUT_KEY_4,
	INPUT_KEY_5,
	INPUT_KEY_6,
	INPUT_KEY_7,
	INPUT_KEY_8,
	INPUT_KEY_9,
	INPUT_KEY_0,
	INPUT_KEY_MINUS,
	INPUT_KEY_EQUAL,
	INPUT_KEY_BACKSPACE,
	INPUT_KEY_TAB,
	INPUT_KEY_Q,
	INPUT_KEY_W,
	INPUT_KEY_E,
	INPUT_KEY_R,
	INPUT_KEY_T,
	INPUT_KEY_Y,
	INPUT_KEY_U,
	INPUT_KEY_I,
	INPUT_KEY_O,
	INPUT_KEY_P,
	INPUT_KEY_LEFT_BRACKET,
	INPUT_KEY_RIGHT_BRACKET,
	INPUT_KEY_BACKSLASH,
	INPUT_KEY_CAPS_LOCK,
	INPUT_KEY_A,
	INPUT_KEY_S,
	INPUT_KEY_D,
	INPUT_KEY_F,
	INPUT_KEY_G,
	INPUT_KEY_H,
	INPUT_KEY_J,
	INPUT_KEY_K,
	INPUT_KEY_L,
	INPUT_KEY_SEMICOLON,
	INPUT_KEY_APOSTROPHE,
	INPUT_KEY_ENTER,
	INPUT_KEY_LEFT_SHIFT,
	INPUT_KEY_NON_US_BACKSLASH,
	INPUT_KEY_Z,
	INPUT_KEY_X,
	INPUT_KEY_C,
	INPUT_KEY_V,
	INPUT_KEY_B,
	INPUT_KEY_N,
	INPUT_KEY_M,
	INPUT_KEY_COMMA,
	INPUT_KEY_PERIOD,
	INPUT_KEY_SLASH,
	INPUT_KEY_RIGHT_SHIFT,
	INPUT_KEY_LEFT_CONTROL,
	INPUT_KEY_LEFT_META,
	INPUT_KEY_LEFT_ALT,
	INPUT_KEY_SPACE,
	INPUT_KEY_RIGHT_ALT,
	INPUT_KEY_RIGHT_META,
	INPUT_KEY_MENU,
	INPUT_KEY_RIGHT_CONTROL,
	INPUT_KEY_INSERT,
	INPUT_KEY_HOME,
	INPUT_KEY_PAGE_UP,
	INPUT_KEY_DELETE,
	INPUT_KEY_END,
	INPUT_KEY_PAGE_DOWN,
	INPUT_KEY_UP,
	INPUT_KEY_LEFT,
	INPUT_KEY_DOWN,
	INPUT_KEY_RIGHT,
	INPUT_KEY_NUM_LOCK,
	INPUT_KEY_KEYPAD_DIVIDE,
	INPUT_KEY_KEYPAD_MULTIPLY,
	INPUT_KEY_KEYPAD_SUBTRACT,
	INPUT_KEY_KEYPAD_7,
	INPUT_KEY_KEYPAD_8,
	INPUT_KEY_KEYPAD_9,
	INPUT_KEY_KEYPAD_ADD,
	INPUT_KEY_KEYPAD_4,
	INPUT_KEY_KEYPAD_5,
	INPUT_KEY_KEYPAD_6,
	INPUT_KEY_KEYPAD_1,
	INPUT_KEY_KEYPAD_2,
	INPUT_KEY_KEYPAD_3,
	INPUT_KEY_KEYPAD_ENTER,
	INPUT_KEY_KEYPAD_0,
	INPUT_KEY_KEYPAD_DECIMAL,
};

/* Pointer axes currently normalized by input producers. */
enum input_axis_code {
	INPUT_AXIS_INVALID = 0,
	INPUT_AXIS_X,
	INPUT_AXIS_Y,
	INPUT_AXIS_VERTICAL_SCROLL,
	INPUT_AXIS_HORIZONTAL_SCROLL,
};

/* Pointer buttons currently normalized by input producers. */
enum input_button_code {
	INPUT_BUTTON_INVALID = 0,
	INPUT_BUTTON_PRIMARY,
	INPUT_BUTTON_SECONDARY,
	INPUT_BUTTON_MIDDLE,
	INPUT_BUTTON_BACK,
	INPUT_BUTTON_FORWARD,
};

/* Immutable description of one producer-local control ID. */
struct input_control_descriptor {
	uint32_t                id;
	enum input_control_kind kind;
	uint32_t                code;
	uint32_t                group_id;
	int64_t                 logical_minimum;
	int64_t                 logical_maximum;
};

/* Value representation used by one synchronous device output. */
enum input_output_kind {
	INPUT_OUTPUT_KIND_INVALID = 0,
	INPUT_OUTPUT_KIND_BOOLEAN,
	INPUT_OUTPUT_KIND_BOUNDED_VALUE,
};

/* Project-defined output functions currently understood by input clients. */
enum input_output_code {
	INPUT_OUTPUT_CODE_INVALID = 0,
	INPUT_OUTPUT_CODE_NUM_LOCK_INDICATOR,
	INPUT_OUTPUT_CODE_CAPS_LOCK_INDICATOR,
	INPUT_OUTPUT_CODE_SCROLL_LOCK_INDICATOR,
};

/* Immutable description of one producer-local output ID. */
struct input_output_descriptor {
	uint32_t               id;
	enum input_output_kind kind;
	uint32_t               code;
	uint32_t               group_id;
	int64_t                logical_minimum;
	int64_t                logical_maximum;
};

/* One requested value in an atomic output batch. */
struct input_output_value {
	uint32_t output_id;
	uint32_t reserved;
	int64_t  value;
};

/* Record kinds retained in the ordered event history. */
enum input_event_type {
	INPUT_EVENT_TYPE_INVALID = 0,
	INPUT_EVENT_TYPE_CONTROL,
	INPUT_EVENT_TYPE_STATE_RESET,
};

/* Flags attached to one retained event record. */
enum input_event_flags {
	INPUT_EVENT_FLAG_NONE      = 0u,
	INPUT_EVENT_FLAG_FRAME_END = 1u << 0u,
};

/* One value update or state reset within a coherent input frame. */
struct input_event {
	uint64_t              sequence;
	uint64_t              frame;
	enum input_event_type type;
	uint32_t              control_id;
	int64_t               value;
	uint32_t              flags;
	uint32_t              reserved;
};

/* Operations accepted by every input-device service capability. */
enum input_op {
	INPUT_OP_GET_INFO = 0,
	INPUT_OP_READ_DESCRIPTORS,
	INPUT_OP_READ_EVENTS,
	INPUT_OP_ACQUIRE_EVENT_SIGNAL,
	INPUT_OP_READ_OUTPUT_DESCRIPTORS,
	INPUT_OP_SET_OUTPUTS,
};

/* Common operation header for input-service requests. */
struct input_request_header {
	enum input_op op;
};

/* Request immutable service metadata. */
struct input_get_info_request {
	struct input_request_header header;
};

/* Immutable metadata returned by INPUT_OP_GET_INFO. */
struct input_get_info_response {
	enum input_device_class device_class;
	uint32_t                descriptor_count;
	uint32_t                output_descriptor_count;
	uint32_t                queue_capacity;
	uint32_t                maximum_frame_events;
	uint32_t                maximum_output_values;
};

/* Request a provider-ordered page of immutable control descriptors. */
struct input_read_descriptors_request {
	struct input_request_header header;
	uint32_t                    reserved;
	uint64_t                    offset;
	uint64_t                    count;
};

/* Descriptor entries follow this successful response header. */
struct input_read_descriptors_response {
	uint64_t                        total;
	uint64_t                        returned;
	struct input_control_descriptor descriptors[];
};

/* Request retained records strictly newer than after_sequence. */
struct input_read_events_request {
	struct input_request_header header;
	uint32_t                    reserved;
	uint64_t                    after_sequence;
	uint64_t                    count;
};

/* Conditions reported with a successful retained-event read. */
enum input_read_event_flags {
	INPUT_READ_EVENT_FLAG_NONE = 0u,
	INPUT_READ_EVENT_FLAG_GAP  = 1u << 0u,
};

/* Event records follow this successful response header. */
struct input_read_events_response {
	uint64_t           oldest_sequence;
	uint64_t           newest_sequence;
	uint64_t           returned;
	uint32_t           flags;
	uint32_t           reserved;
	struct input_event events[];
};

/* Request the coalesced signal used to announce newly retained events. */
struct input_acquire_event_signal_request {
	struct input_request_header header;
};

/* Event signal delegated to the caller with exactly CAP_WAIT. */
struct input_acquire_event_signal_response {
	cap_id_t capability;
};

/* Request a provider-ordered page of immutable output descriptors. */
struct input_read_output_descriptors_request {
	struct input_request_header header;
	uint32_t                    reserved;
	uint64_t                    offset;
	uint64_t                    count;
};

/* Output descriptor entries follow this successful response header. */
struct input_read_output_descriptors_response {
	uint64_t                       total;
	uint64_t                       returned;
	struct input_output_descriptor descriptors[];
};

/* Apply one nonempty atomic batch of producer-local output values. */
struct input_set_outputs_request {
	struct input_request_header header;
	uint32_t                    reserved;
	uint64_t                    count;
	struct input_output_value   values[];
};
