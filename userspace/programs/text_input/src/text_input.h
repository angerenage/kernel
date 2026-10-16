#pragma once

#include <base/channel.h>
#include <base/process.h>
#include <protocol/keyboard.h>
#include <protocol/text_input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h"

#define TEXT_INPUT_EVENT_QUEUE_CAPACITY 4096u
#define TEXT_INPUT_MAXIMUM_FRAME_EVENTS 128u
#define TEXT_INPUT_OBJECT_ID 1u
#define TEXT_INPUT_KEYBOARD_EVENT_PAGE_SIZE 64u

/* Per-keyboard transform context and public composition identity. */
struct keyboard_text_state {
	struct keyboard_text_state* next;
	uint64_t                    id;
	uint64_t                    source_id;
	uint64_t                    composition_id;
	uint8_t                     context[TEXT_LAYOUT_CONTEXT_CAPACITY];
	size_t                      context_size;
};

/* One logical action published atomically with the other actions in a frame. */
struct text_input_action {
	enum text_input_event_type type;
	uint64_t                   source_id;
	uint64_t                   composition_id;
	const uint8_t*             text;
	size_t                     text_size;
	uint32_t                   selection_start;
	uint32_t                   selection_end;
	uint32_t                   delete_before;
	uint32_t                   delete_after;
	enum text_input_key_code   key;
	enum text_input_key_action key_action;
	uint32_t                   key_modifiers;
};

/* State shared by the text translator and its capability-server thread. */
struct text_input {
	process_id_t                pid;
	cap_id_t                    self_cap;
	channel_id_t                endpoint;
	cap_id_t                    channel_activity_cap;
	cap_id_t                    service_cap;
	cap_id_t                    event_signal_cap;
	cap_id_t                    keyboard_cap;
	cap_id_t                    keyboard_signal_cap;
	uint64_t                    keyboard_cursor;
	uint64_t                    next_sequence;
	uint64_t                    next_frame;
	uint64_t                    next_source_id;
	uint64_t                    next_composition_id;
	bool                        locked;
	bool                        advertised;
	struct text_layout          layout;
	struct keyboard_text_state* keyboards;
	struct keyboard_event       keyboard_events[TEXT_INPUT_KEYBOARD_EVENT_PAGE_SIZE];
	struct text_input_event*    events;
	size_t                      event_tail;
	size_t                      event_count;
};

/* Load an LDML layout and initialize a service that is advertised once usable. */
bool text_input_initialize(struct text_input* input, const char* layout_path);

/* Allocate a nonzero input-source identifier that is never reused. */
uint64_t text_input_allocate_source_id(struct text_input* input);

/* Allocate a nonzero composition identifier that is never reused. */
uint64_t text_input_allocate_composition_id(struct text_input* input);

/* Retain one atomic frame and notify all waiting consumers. */
bool text_input_publish_actions(struct text_input* input, const struct text_input_action* actions, size_t action_count);

/* Translate keyboard events until an unrecoverable service failure. */
int text_input_run(struct text_input* input);

/* Serialize state shared with the capability server. */
void text_input_lock(struct text_input* input);

/* Release serialized text-input state. */
void text_input_unlock(struct text_input* input);

/* Start the text-input capability server on a detached thread. */
bool text_input_server_start(struct text_input* input);
