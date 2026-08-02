#pragma once

#include <base/cap.h>
#include <base/signal.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stdint.h>

/* Create a Signal capability with full synchronous rights. */
syscall_status_t signal_create(cap_id_t* out_cap);

/* Publish a payload and optionally return receiver and delivery counts. */
syscall_status_t signal_send(cap_id_t cap, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                             struct signal_send_response* out_response);

/* Read the remembered message without creating or advancing a synchronous receiver. */
syscall_status_t signal_read(cap_id_t cap, struct signal_message* out_message, bool* out_has_value);

/* Block until the calling thread receives a new message. */
syscall_status_t signal_wait(cap_id_t cap, struct signal_message* out_message);

/* Consume a new message without blocking. */
syscall_status_t signal_try_wait(cap_id_t cap, struct signal_message* out_message, bool* out_received);

/* Remove the calling thread's synchronous receiver subscription. */
syscall_status_t signal_unsubscribe(cap_id_t cap);

/* Destroy the Signal and invalidate every capability that references it. */
syscall_status_t signal_destroy(cap_id_t cap);
