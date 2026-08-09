#pragma once

#include <base/signal.h>
#include <base/upcall.h>
#include <core/capability.h>
#include <core/spinlock.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct signal_handler_binding;
struct signal_wait_binding;
struct uthread;

/* Multi-producer broadcast object carrying one authenticated four-word message. */
struct signal {
	struct spinlock                lock;
	signal_id_t                    id;
	struct signal_message          latest;
	uint64_t                       generation;
	struct signal_handler_binding* handler_head;
	struct signal_handler_binding* handler_tail;
	struct signal_handler_binding* handler_wake_head;
	struct signal_handler_binding* handler_wake_tail;
	size_t                         handler_count;
	struct signal_wait_binding*    wait_head;
	struct signal_wait_binding*    wait_tail;
	size_t                         wait_receiver_count;
	struct thread_wait_queue       waiters;
	/* Lazily-created cap_object id for this Signal. */
	cap_object_id_t cap_object_id;
	uint64_t        reference_count;
	bool            has_value;
	bool            closing;
};

/* Create and register a signal. The returned pointer owns the registry reference. */
struct signal* signal_create(void);

/* Acquire a registered signal reference, rejecting closing signals. */
struct signal* signal_acquire(signal_id_t id);

/* Retain an existing signal reference. */
bool signal_retain(struct signal* signal);

/* Drop a reference acquired through signal_acquire() or signal_create(). */
void signal_release(struct signal* signal);

/*
 * Remove a signal from the registry, purge queued handler requests, wake waiters,
 * detach receivers, and consume its owner reference.
 */
enum signal_result signal_destroy(struct signal* signal);

/*
 * Publish one value to every active receiver without allocating or sleeping.
 * out_receiver_count counts registered handlers plus wait calls blocked at the
 * publication point. out_delivery_count counts successful upcall enqueues plus
 * wait calls for which a private wake value was installed.
 */
enum signal_result signal_send(struct signal* signal, process_id_t sender, const struct signal_payload* payload,
                               uint64_t* out_receiver_count, uint64_t* out_delivery_count);

/*
 * Publish like signal_send(), but replace only older coalescible publications
 * pending for the same handler. Ordinary queued publications are never removed.
 */
enum signal_result signal_send_coalesced(struct signal* signal, process_id_t sender,
                                         const struct signal_payload* payload, uint64_t* out_receiver_count,
                                         uint64_t* out_delivery_count);

/*
 * Publish atomically to all live handlers, reserving queue admission first. A
 * forced handler request may evict the oldest evictable pending upcall and is
 * itself non-evictable. If any handler cannot reserve admission, nothing is
 * published and no receiver is modified.
 */
enum signal_result signal_send_force(struct signal* signal, process_id_t sender, const struct signal_payload* payload,
                                     uint64_t* out_receiver_count, uint64_t* out_delivery_count);

/* Read the currently remembered message without creating a receiver or consuming a generation. */
enum signal_result signal_read(struct signal* signal, struct signal_message* out_message);

/* Return the newest message not yet consumed by the current uthread, without blocking. */
enum signal_result signal_try_wait(struct signal* signal, struct signal_message* out_message);

/* Block the current uthread until it has a new message, the signal closes, or waiting is canceled. */
enum signal_result signal_wait(struct signal* signal, struct signal_message* out_message);

/* Remove the current uthread's synchronous receiver and discard its generation cursor. */
enum signal_result signal_unregister_wait_receiver(struct signal* signal);

/*
 * Register one userspace upcall handler for target with immutable lifecycle flags.
 * A one-shot binding is removed after its first successfully admitted upcall.
 */
enum signal_result signal_register_handler(struct signal* signal, struct uthread* target, user_upcall_entry_t* handler,
                                           uint32_t flags);

/* Remove target's persistent userspace handler registration and purge its queued requests. */
enum signal_result signal_unregister_handler(struct signal* signal, struct uthread* target);

/* Remove every handler and synchronous receiver owned by a uthread beginning destruction. */
void signal_unregister_thread_receivers(struct uthread* target);

/* Introspection helpers used by diagnostics and tests. */
bool signal_has_value(struct signal* signal);

/* Return a stable signal identifier, or SIGNAL_ID_INVALID for NULL. */
signal_id_t signal_id(const struct signal* signal);

/* Return the lazily-created cap_object id for signal. */
cap_object_id_t signal_cap_object_id(const struct signal* signal);

/* Publish a cap_object id on an open signal. */
bool signal_set_cap_object_id(struct signal* signal, cap_object_id_t id);

/* Destroy the lazily-created cap_object for signal and clear the slot. */
bool signal_destroy_cap_object(struct signal* signal);

/* Return the current generation number, which monotonically increases with each publication. */
uint64_t signal_generation(struct signal* signal);

/* Return the number of registered handlers for a signal. */
size_t signal_handler_count(struct signal* signal);

/* Return the number of synchronous receiver subscriptions for a signal. */
size_t signal_wait_subscription_count(struct signal* signal);

/* Return the number of threads waiting on a signal. */
size_t signal_blocked_waiter_count(struct signal* signal);

/* Return the total number of active signals. */
size_t signal_count(void);
