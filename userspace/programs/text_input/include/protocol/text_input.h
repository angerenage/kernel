#pragma once

/*
 * System Unicode text-input protocol.
 *
 * The text-input service publishes committed text and replaceable pre-edit
 * state as retained, framed UTF-8 records. It does not expose keyboard layout
 * positions or make application-focus decisions.
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
 * provisional state for its source. No Unicode normalization is implied.
 */
struct text_input_event {
	uint64_t                   sequence;
	uint64_t                   frame;
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
