#pragma once

#include <base/message.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stddef.h>

/* Send a message payload to the process identified by pid. */
syscall_status_t message_send(process_id_t pid, const void* buffer, size_t length);

/* Receive the next message. */
syscall_status_t message_recv(void* buffer, size_t buffer_size, size_t* out_length, process_id_t* out_sender_pid,
                              size_t* out_required_size);
