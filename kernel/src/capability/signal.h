#pragma once

#include <base/cap.h>
#include <base/signal.h>
#include <base/syscall.h>
#include <core/signal.h>

/* Grant recipient a Signal capability for target with the specified rights. */
cap_id_t kernel_signal_grant(struct signal* target, process_id_t recipient, cap_rights_t rights);

/* Grant recipient a Signal capability with all synchronous rights. */
cap_id_t kernel_signal_grant_full(struct signal* target, process_id_t recipient);

/* Send a payload through an authorized Signal capability. */
syscall_result_t kernel_signal_send(cap_id_t cap, process_id_t caller, const struct signal_payload* payload,
                                    uint32_t flags, struct signal_send_response* out_response);

/* Read a queued message through an authorized Signal capability. */
syscall_result_t kernel_signal_read(cap_id_t cap, process_id_t caller, struct signal_message* out_message);

/* Wait for a message through an authorized Signal capability. */
syscall_result_t kernel_signal_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message);

/* Try to receive a message through an authorized Signal capability. */
syscall_result_t kernel_signal_try_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message);
