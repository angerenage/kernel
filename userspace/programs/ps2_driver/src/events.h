#pragma once

#include <base/syscall.h>
#include <protocol/input.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ps2.h"

/* Producer-local IDs used by the PS/2 keyboard's output descriptors. */
enum ps2_keyboard_output_id {
	PS2_KEYBOARD_OUTPUT_NUM_LOCK = 1u,
	PS2_KEYBOARD_OUTPUT_CAPS_LOCK,
	PS2_KEYBOARD_OUTPUT_SCROLL_LOCK,
};

/* Create and advertise the normalized input service after identification. */
bool ps2_events_initialize(enum ps2_device_kind kind, uint8_t mouse_id, cap_id_t driver_activity_cap);

/* Report whether a reidentified device still matches the advertised service. */
bool ps2_events_match(enum ps2_device_kind kind, uint8_t mouse_id);

/* Publish one decoded keyboard transition through the input service. */
bool ps2_event_key(uint8_t scan_code, bool extended, bool pressed);

/* Publish the make-only Pause key as separate press and release frames. */
bool ps2_event_pause(void);

/* Publish one decoded relative mouse packet as a coherent input frame. */
bool ps2_event_mouse(int16_t delta_x, int16_t delta_y, int8_t wheel, uint8_t buttons);

/* Publish a discontinuity and clear the producer's remembered control state. */
bool ps2_events_reset(void);

/* Report whether an event publication failure made the input service unusable. */
bool ps2_events_failed(void);

/* Take the next validated output batch for execution by the transport owner. */
bool ps2_outputs_take(struct input_output_value* values, size_t capacity, size_t* out_count);

/* Complete the output batch currently owned by the transport thread. */
bool ps2_outputs_complete(syscall_status_t status);
