#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/text_input.h>
#include <runtime/registry.h>
#include <stddef.h>
#include <stdint.h>

/* Metadata accompanying one validated text-event page. */
struct text_input_event_page {
	uint64_t oldest_sequence;
	uint64_t newest_sequence;
	size_t   returned;
	uint32_t flags;
};

/* Acquire the default system text-input service through init. */
struct registry_call_result text_input_acquire(struct registry_service_handle* out_handle);

/* Read the text-input service's limits and retained-stream cursors. */
syscall_status_t text_input_get_info(cap_id_t text_input_cap, struct text_input_get_info_response* out_info);

/* Read complete text-input frames newer than after_sequence. */
syscall_status_t text_input_read_events(cap_id_t text_input_cap, uint64_t after_sequence,
                                        struct text_input_event* events, size_t capacity,
                                        struct text_input_event_page* out_page);

/* Acquire the text-input service's coalesced event signal with CAP_WAIT. */
syscall_status_t text_input_acquire_event_signal(cap_id_t text_input_cap, cap_id_t* out_signal_cap);
