#pragma once

#include <base/cap.h>
#include <base/signal.h>
#include <base/syscall.h>
#include <core/signal.h>

/* Grant recipient a capability for target with the specified rights. */
cap_id_t kernel_signal_grant(struct signal* target, process_id_t recipient, cap_rights_t rights);

/* Grant full synchronous Signal rights to recipient. */
cap_id_t kernel_signal_grant_full(struct signal* target, process_id_t recipient);

/* Fast-path operations that resolve and authorize a Signal capability. */
syscall_result_t kernel_signal_send(cap_id_t cap, process_id_t caller, const struct signal_payload* payload,
                                    uint32_t flags, struct signal_send_response* out_response);
syscall_result_t kernel_signal_read(cap_id_t cap, process_id_t caller, struct signal_message* out_message);
syscall_result_t kernel_signal_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message);
syscall_result_t kernel_signal_try_wait(cap_id_t cap, process_id_t caller, struct signal_message* out_message);
