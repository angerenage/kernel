#pragma once

#include <base/channel.h>
#include <base/process.h>
#include <protocol/input_manager.h>
#include <protocol/keyboard.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYBOARD_CONTROLLER_EVENT_QUEUE_CAPACITY 4096u
#define KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS 128u
#define KEYBOARD_CONTROLLER_MAXIMUM_KEYBOARDS 1024u
#define KEYBOARD_CONTROLLER_REPEAT_DELAY_MS 500u
#define KEYBOARD_CONTROLLER_REPEAT_INTERVAL_MS 33u
#define KEYBOARD_CONTROLLER_REPEAT_QUANTUM_MS 10u
#define KEYBOARD_CONTROLLER_OBJECT_ID 1u

struct keyboard_output {
	uint32_t output_id;
	uint32_t lock;
};

/* One currently attached keyboard and its controller-owned physical state. */
struct managed_keyboard {
	struct managed_keyboard* next;
	uint64_t                 id;
	uint64_t                 source_id;
	struct keyboard_output*  outputs;
	size_t                   output_count;
	uint64_t                 press_order[INPUT_KEY_KEYPAD_DECIMAL + 1u];
	bool                     pressed[INPUT_KEY_KEYPAD_DECIMAL + 1u];
	uint32_t                 modifiers;
	uint32_t                 locks;
	enum input_key_code      repeat_key;
	uint32_t                 repeat_remaining_ms;
	bool                     seen;
	bool                     led_dirty;
};

/* One input-manager source rejected until that source identity disappears. */
struct rejected_keyboard_source {
	struct rejected_keyboard_source* next;
	uint64_t                         source_id;
	bool                             seen;
};

/* State shared by the serialized controller, timer, and capability server. */
struct keyboard_controller {
	process_id_t                     pid;
	cap_id_t                         self_cap;
	channel_id_t                     endpoint;
	cap_id_t                         channel_activity_cap;
	cap_id_t                         service_cap;
	cap_id_t                         event_signal_cap;
	cap_id_t                         activity_cap;
	cap_id_t                         registry_signal_cap;
	cap_id_t                         repeat_activity_cap;
	uint64_t                         registry_subscription;
	bool                             registry_dirty;
	bool                             registry_failed;
	bool                             manager_dirty;
	bool                             manager_dead;
	bool                             manager_stopping;
	bool                             failed;
	bool                             locked;
	cap_id_t                         manager_cap;
	cap_id_t                         manager_signal_cap;
	cap_id_t                         manager_waiter_cap;
	process_id_t                     manager_owner;
	uint64_t                         manager_topology_generation;
	uint64_t                         manager_cursor;
	uint64_t                         topology_generation;
	uint64_t                         next_keyboard_id;
	uint64_t                         next_sequence;
	uint64_t                         next_frame;
	uint64_t                         next_press_order;
	struct keyboard_event*           events;
	size_t                           event_tail;
	size_t                           event_count;
	struct managed_keyboard*         keyboards;
	size_t                           keyboard_count;
	struct rejected_keyboard_source* rejected_sources;
	struct input_manager_event       pending[KEYBOARD_CONTROLLER_MAXIMUM_FRAME_EVENTS];
	size_t                           pending_count;
	uint64_t                         pending_frame;
	uint64_t                         pending_source;
};

/* Initialize and advertise an empty keyboard controller. */
bool keyboard_controller_initialize(struct keyboard_controller* controller);

/* Aggregate keyboard state until an unrecoverable controller failure. */
int keyboard_controller_run(struct keyboard_controller* controller);

/* Serialize state shared with client and repeat threads. */
void keyboard_controller_lock(struct keyboard_controller* controller);

/* Release serialized controller access. */
void keyboard_controller_unlock(struct keyboard_controller* controller);

/* Start the keyboard capability server on a detached thread. */
bool keyboard_controller_server_start(struct keyboard_controller* controller);
