#pragma once

#include <base/cap.h>
#include <base/signal.h>
#include <base/syscall.h>
#include <base/upcall.h>
#include <stdbool.h>
#include <stdint.h>

/* Create a Signal capability with full synchronous rights. */
syscall_status_t signal_create(cap_id_t* out_cap);

/* Publish a payload with the requested userspace delivery behavior. */
syscall_status_t signal_send(cap_id_t cap, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint32_t flags,
                             struct signal_send_response* out_response);

/* Read the remembered message without creating or advancing a synchronous receiver. */
syscall_status_t signal_read(cap_id_t cap, struct signal_message* out_message, bool* out_has_value);

/* Read Signal state and calling-thread upcall accounting through CAP_CALL | CAP_READ. */
syscall_status_t signal_read_info(cap_id_t cap, struct signal_read_response* out_response);

/* Block until the calling thread receives a new message. */
syscall_status_t signal_wait(cap_id_t cap, struct signal_message* out_message);

/* Consume a new message without blocking. */
syscall_status_t signal_try_wait(cap_id_t cap, struct signal_message* out_message, bool* out_received);

/* Register the calling thread's asynchronous handler with immutable lifecycle flags. */
syscall_status_t signal_set_handler(cap_id_t cap, user_upcall_entry_t* handler, uint32_t flags);

/* Remove the calling thread's persistent handler through CAP_CALL | CAP_MAP. */
syscall_status_t signal_clear_handler(cap_id_t cap);

/* Remove the calling thread's synchronous receiver subscription. */
syscall_status_t signal_unsubscribe(cap_id_t cap);

/* Destroy the Signal and invalidate every capability that references it. */
syscall_status_t signal_destroy(cap_id_t cap);
