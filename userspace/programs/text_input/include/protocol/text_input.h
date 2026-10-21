#pragma once

/*
 * System Unicode text-input protocol.
 *
 * The text-input service publishes committed text, replaceable pre-edit state,
 * and layout-resolved logical key transitions as retained, framed records. It
 * does not expose physical keyboard positions or make application-focus
 * decisions.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <stdint.h>

/* Registry identity of the single system-wide text-input service. */
#define TEXT_INPUT_NAMESPACE "input"
#define TEXT_INPUT_PROTOCOL_NAME "text"
#define TEXT_INPUT_SERVICE_NAME "default"

/* Exact rights granted to text-input clients and notification signals. */
#define TEXT_INPUT_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))
#define TEXT_INPUT_EVENT_SIGNAL_RIGHTS ((cap_rights_t)CAP_WAIT)

/* Number of UTF-8 bytes carried by one fixed-size retained record. */
#define TEXT_INPUT_EVENT_TEXT_CAPACITY 32u

/* Logical actions represented by one or more retained records. */
enum text_input_event_type {
	TEXT_INPUT_EVENT_INVALID = 0,
	TEXT_INPUT_EVENT_RESET,
	TEXT_INPUT_EVENT_PREEDIT,
	TEXT_INPUT_EVENT_COMMIT,
	TEXT_INPUT_EVENT_DELETE_SURROUNDING,
	TEXT_INPUT_EVENT_KEY,
};

/* Layout-resolved or non-textual identity of one key transition. */
enum text_input_key_code {
	TEXT_INPUT_KEY_INVALID = 0,
	TEXT_INPUT_KEY_TEXT,
	TEXT_INPUT_KEY_ESCAPE,
	TEXT_INPUT_KEY_F1,
	TEXT_INPUT_KEY_F2,
	TEXT_INPUT_KEY_F3,
	TEXT_INPUT_KEY_F4,
	TEXT_INPUT_KEY_F5,
	TEXT_INPUT_KEY_F6,
	TEXT_INPUT_KEY_F7,
	TEXT_INPUT_KEY_F8,
	TEXT_INPUT_KEY_F9,
	TEXT_INPUT_KEY_F10,
	TEXT_INPUT_KEY_F11,
	TEXT_INPUT_KEY_F12,
	TEXT_INPUT_KEY_PRINT_SCREEN,
	TEXT_INPUT_KEY_SCROLL_LOCK,
	TEXT_INPUT_KEY_PAUSE,
	TEXT_INPUT_KEY_BACKSPACE,
	TEXT_INPUT_KEY_TAB,
	TEXT_INPUT_KEY_ENTER,
	TEXT_INPUT_KEY_CAPS_LOCK,
	TEXT_INPUT_KEY_LEFT_SHIFT,
	TEXT_INPUT_KEY_RIGHT_SHIFT,
	TEXT_INPUT_KEY_LEFT_CONTROL,
	TEXT_INPUT_KEY_RIGHT_CONTROL,
	TEXT_INPUT_KEY_LEFT_ALT,
	TEXT_INPUT_KEY_RIGHT_ALT,
	TEXT_INPUT_KEY_LEFT_META,
	TEXT_INPUT_KEY_RIGHT_META,
	TEXT_INPUT_KEY_MENU,
	TEXT_INPUT_KEY_INSERT,
	TEXT_INPUT_KEY_HOME,
	TEXT_INPUT_KEY_PAGE_UP,
	TEXT_INPUT_KEY_DELETE,
	TEXT_INPUT_KEY_END,
	TEXT_INPUT_KEY_PAGE_DOWN,
	TEXT_INPUT_KEY_UP,
	TEXT_INPUT_KEY_LEFT,
	TEXT_INPUT_KEY_DOWN,
	TEXT_INPUT_KEY_RIGHT,
	TEXT_INPUT_KEY_CENTER,
	TEXT_INPUT_KEY_NUM_LOCK,
};

/* Transition represented by one logical key action. */
enum text_input_key_action {
	TEXT_INPUT_KEY_ACTION_INVALID = 0,
	TEXT_INPUT_KEY_ACTION_PRESSED,
	TEXT_INPUT_KEY_ACTION_RELEASED,
	TEXT_INPUT_KEY_ACTION_REPEATED,
};

/* Physical modifier keys held when a logical key transition is published. */
enum text_input_key_modifier {
	TEXT_INPUT_KEY_MODIFIER_NONE          = 0u,
	TEXT_INPUT_KEY_MODIFIER_LEFT_SHIFT    = 1u << 0u,
	TEXT_INPUT_KEY_MODIFIER_RIGHT_SHIFT   = 1u << 1u,
	TEXT_INPUT_KEY_MODIFIER_LEFT_CONTROL  = 1u << 2u,
	TEXT_INPUT_KEY_MODIFIER_RIGHT_CONTROL = 1u << 3u,
	TEXT_INPUT_KEY_MODIFIER_LEFT_ALT      = 1u << 4u,
	TEXT_INPUT_KEY_MODIFIER_RIGHT_ALT     = 1u << 5u,
	TEXT_INPUT_KEY_MODIFIER_LEFT_META     = 1u << 6u,
	TEXT_INPUT_KEY_MODIFIER_RIGHT_META    = 1u << 7u,
};

/* Boundaries attached to one retained text-input record. */
enum text_input_event_flags {
	TEXT_INPUT_EVENT_FLAG_NONE      = 0u,
	TEXT_INPUT_EVENT_FLAG_TEXT_END  = 1u << 0u,
	TEXT_INPUT_EVENT_FLAG_FRAME_END = 1u << 1u,
};

/*
 * One chunk of an ordered text-input action.
 *
 * source_id is a nonzero service-local input origin and is never reused.
 * composition_id is nonzero for PREEDIT, and a COMMIT with that same ID ends
 * the pre-edit. A zero composition ID denotes text unrelated to a pre-edit.
 * PREEDIT records replace the complete current pre-edit. selection_start and
 * selection_end are UTF-8 boundary byte offsets within that complete text.
 * Empty pre-edit text cancels the composition. COMMIT records contain
 * immutable text to insert. DELETE_SURROUNDING uses delete_before and
 * delete_after as UTF-8 byte counts and carries no text. RESET discards all
 * provisional state for its source. KEY describes a press, release, or repeat
 * after applying the keyboard layout. TEXT keys carry their layout-resolved
 * label in text; other keys carry no text. Key labels identify shortcuts and
 * are not committed text. A KEY and the PREEDIT or COMMIT actions caused by
 * the same transition share one frame and the originating monotonic
 * timestamp. Clients process the complete frame to resolve shortcuts and
 * composition handling before accepting text. No Unicode normalization is
 * implied.
 */
struct text_input_event {
	uint64_t                   sequence;
	uint64_t                   frame;
	uint64_t                   timestamp_ns;
	uint64_t                   source_id;
	uint64_t                   composition_id;
	enum text_input_event_type type;
	uint32_t                   flags;
	uint32_t                   text_offset;
	uint32_t                   text_size;
	uint32_t                   text_total_size;
	uint32_t                   selection_start;
	uint32_t                   selection_end;
	uint32_t                   delete_before;
	uint32_t                   delete_after;
	enum text_input_key_code   key;
	enum text_input_key_action key_action;
	uint32_t                   key_modifiers;
	uint32_t                   reserved;
	uint8_t                    text[TEXT_INPUT_EVENT_TEXT_CAPACITY];
};

/* Operations accepted by the system text-input capability. */
enum text_input_op {
	TEXT_INPUT_OP_GET_INFO = 0,
	TEXT_INPUT_OP_READ_EVENTS,
	TEXT_INPUT_OP_ACQUIRE_EVENT_SIGNAL,
};

/* Common operation header for text-input requests. */
struct text_input_request_header {
	enum text_input_op op;
};

/* Request retained-stream metadata. */
struct text_input_get_info_request {
	struct text_input_request_header header;
};

/* Current immutable limits and retained-stream cursors. */
struct text_input_get_info_response {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	uint32_t queue_capacity;
	uint32_t maximum_frame_events;
	uint32_t text_capacity_per_event;
	uint32_t reserved;
};

/* Request complete frames newer than after_sequence. */
struct text_input_read_events_request {
	struct text_input_request_header header;
	uint32_t                         reserved;
	uint64_t                         after_sequence;
	uint64_t                         count;
};

/* Conditions reported with a successful text-event read. */
enum text_input_read_event_flags {
	TEXT_INPUT_READ_EVENT_FLAG_NONE = 0u,
	TEXT_INPUT_READ_EVENT_FLAG_GAP  = 1u << 0u,
};

/* Complete retained frames follow this successful response header. */
struct text_input_read_events_response {
	uint64_t                oldest_sequence;
	uint64_t                newest_sequence;
	uint64_t                returned;
	uint32_t                flags;
	uint32_t                reserved;
	struct text_input_event events[];
};

/* Request the coalesced signal announcing newly retained frames. */
struct text_input_acquire_event_signal_request {
	struct text_input_request_header header;
};

/* Event signal delegated to the caller with exactly CAP_WAIT. */
struct text_input_acquire_event_signal_response {
	cap_id_t capability;
};
