#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/keyboard.h>
#include <runtime/init.h>
#include <stddef.h>
#include <stdint.h>

/* Metadata accompanying one validated keyboard-state snapshot. */
struct keyboard_state {
	uint64_t sequence;
	uint64_t keyboard_id;
	uint32_t modifiers;
	uint32_t locks;
	size_t   pressed_count;
};

/* Metadata accompanying one validated keyboard-event page. */
struct keyboard_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Acquire the default system keyboard service through init. */
struct init_call_result keyboard_acquire(struct init_service_handle* out_handle);

/* Read controller metadata and the current coherent topology cursor. */
syscall_status_t keyboard_get_info(cap_id_t keyboard_cap, struct keyboard_get_info_response* out_info);

/* Enumerate one page of keyboards from an exact topology generation. */
syscall_status_t keyboard_enumerate(cap_id_t keyboard_cap, uint64_t topology_generation, uint64_t offset,
                                    struct keyboard_info* keyboards, size_t capacity, size_t* out_returned,
                                    uint64_t* out_total);

/* Atomically read one keyboard's physical state into a caller-owned key array. */
syscall_status_t keyboard_read_state(cap_id_t keyboard_cap, uint64_t keyboard_id, enum input_key_code* pressed_keys,
                                     size_t capacity, struct keyboard_state* out_state);

/* Read retained keyboard records newer than after_sequence. */
syscall_status_t keyboard_read_events(cap_id_t keyboard_cap, uint64_t after_sequence, struct keyboard_event* events,
                                      size_t capacity, struct keyboard_event_page* out_page);

/* Acquire the keyboard service's coalesced event signal with CAP_WAIT. */
syscall_status_t keyboard_acquire_event_signal(cap_id_t keyboard_cap, cap_id_t* out_signal_cap);
