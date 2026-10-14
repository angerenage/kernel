#pragma once

/*
 * Layout-independent system keyboard protocol.
 *
 * The keyboard controller exposes attached keyboards, their physical key and
 * modifier state, per-keyboard lock state, and a retained stream of physical
 * key transitions and software repeats. It does not assign text or layout
 * semantics.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <protocol/input.h>
#include <stdint.h>

/* Registry identity of the single system-wide keyboard service. */
#define KEYBOARD_NAMESPACE "input"
#define KEYBOARD_PROTOCOL_NAME "keyboard"
#define KEYBOARD_SERVICE_NAME "default"

/* Exact rights granted to keyboard clients and their notification signals. */
#define KEYBOARD_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))
#define KEYBOARD_EVENT_SIGNAL_RIGHTS ((cap_rights_t)CAP_WAIT)

/* Physical modifier keys held on one keyboard. */
enum keyboard_modifier {
	KEYBOARD_MODIFIER_NONE          = 0u,
	KEYBOARD_MODIFIER_LEFT_SHIFT    = 1u << 0u,
	KEYBOARD_MODIFIER_RIGHT_SHIFT   = 1u << 1u,
	KEYBOARD_MODIFIER_LEFT_CONTROL  = 1u << 2u,
	KEYBOARD_MODIFIER_RIGHT_CONTROL = 1u << 3u,
	KEYBOARD_MODIFIER_LEFT_ALT      = 1u << 4u,
	KEYBOARD_MODIFIER_RIGHT_ALT     = 1u << 5u,
	KEYBOARD_MODIFIER_LEFT_META     = 1u << 6u,
	KEYBOARD_MODIFIER_RIGHT_META    = 1u << 7u,
};

/* Logical lock state maintained independently for one keyboard. */
enum keyboard_lock {
	KEYBOARD_LOCK_NONE        = 0u,
	KEYBOARD_LOCK_CAPS_LOCK   = 1u << 0u,
	KEYBOARD_LOCK_NUM_LOCK    = 1u << 1u,
	KEYBOARD_LOCK_SCROLL_LOCK = 1u << 2u,
};

/* One keyboard in an exact controller-topology generation. */
struct keyboard_info {
	uint64_t id;
};

/* Kinds of records retained by the keyboard controller. */
enum keyboard_event_type {
	KEYBOARD_EVENT_INVALID = 0,
	KEYBOARD_EVENT_ADDED,
	KEYBOARD_EVENT_REMOVED,
	KEYBOARD_EVENT_RESET,
	KEYBOARD_EVENT_KEY,
};

/* Physical transition represented by a key-event record. */
enum keyboard_key_action {
	KEYBOARD_KEY_ACTION_INVALID = 0,
	KEYBOARD_KEY_ACTION_PRESSED,
	KEYBOARD_KEY_ACTION_RELEASED,
	KEYBOARD_KEY_ACTION_REPEATED,
};

/* Flags attached to one retained keyboard record. */
enum keyboard_event_flags {
	KEYBOARD_EVENT_FLAG_NONE      = 0u,
	KEYBOARD_EVENT_FLAG_FRAME_END = 1u << 0u,
};

/* One keyboard lifecycle record or physical key transition. */
struct keyboard_event {
	uint64_t                 sequence;
	uint64_t                 frame;
	uint64_t                 keyboard_id;
	enum keyboard_event_type type;
	enum keyboard_key_action action;
	enum input_key_code      key;
	uint32_t                 modifiers;
	uint32_t                 locks;
	uint32_t                 flags;
	uint32_t                 reserved;
};

/* Operations accepted by the system keyboard capability. */
enum keyboard_op {
	KEYBOARD_OP_GET_INFO = 0,
	KEYBOARD_OP_ENUMERATE,
	KEYBOARD_OP_READ_STATE,
	KEYBOARD_OP_READ_EVENTS,
	KEYBOARD_OP_ACQUIRE_EVENT_SIGNAL,
};

/* Common operation header for keyboard requests. */
struct keyboard_request_header {
	enum keyboard_op op;
};

/* Request controller metadata and a coherent topology cursor. */
struct keyboard_get_info_request {
	struct keyboard_request_header header;
};

/* Current controller metadata returned by KEYBOARD_OP_GET_INFO. */
struct keyboard_get_info_response {
	uint64_t topology_generation;
	uint64_t keyboard_count;
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	uint32_t queue_capacity;
	uint32_t maximum_frame_events;
	uint32_t repeat_delay_ms;
	uint32_t repeat_interval_ms;
};

/* Request one keyboard page from an exact topology generation. */
struct keyboard_enumerate_request {
	struct keyboard_request_header header;
	uint32_t                       reserved;
	uint64_t                       topology_generation;
	uint64_t                       offset;
	uint64_t                       count;
};

/* Keyboard entries follow this successful response header. */
struct keyboard_enumerate_response {
	uint64_t             topology_generation;
	uint64_t             total;
	uint64_t             returned;
	struct keyboard_info keyboards[];
};

/* Request one atomic keyboard-state snapshot. */
struct keyboard_read_state_request {
	struct keyboard_request_header header;
	uint32_t                       reserved;
	uint64_t                       keyboard_id;
};

/* Sorted physical key codes follow this successful response header. */
struct keyboard_read_state_response {
	uint64_t            sequence;
	uint64_t            keyboard_id;
	uint32_t            modifiers;
	uint32_t            locks;
	uint32_t            pressed_count;
	uint32_t            reserved;
	enum input_key_code pressed_keys[];
};

/* Request retained keyboard records newer than after_sequence. */
struct keyboard_read_events_request {
	struct keyboard_request_header header;
	uint32_t                       reserved;
	uint64_t                       after_sequence;
	uint64_t                       count;
};

/* Conditions reported with a successful keyboard-event read. */
enum keyboard_read_event_flags {
	KEYBOARD_READ_EVENT_FLAG_NONE = 0u,
	KEYBOARD_READ_EVENT_FLAG_GAP  = 1u << 0u,
};

/* Retained records follow this successful response header. */
struct keyboard_read_events_response {
	uint64_t              oldest_sequence;
	uint64_t              newest_sequence;
	uint64_t              returned;
	uint32_t              flags;
	uint32_t              reserved;
	struct keyboard_event events[];
};

/* Request the coalesced signal announcing newly retained records. */
struct keyboard_acquire_event_signal_request {
	struct keyboard_request_header header;
};

/* Event signal delegated to the caller with exactly CAP_WAIT. */
struct keyboard_acquire_event_signal_response {
	cap_id_t capability;
};
