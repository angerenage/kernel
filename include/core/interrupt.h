#pragma once

#include <base/interrupt.h>
#include <base/signal.h>
#include <hal/interrupts.h>
#include <stdbool.h>

struct interrupt;
struct signal;

/* Initialize the interrupt core before interrupt objects are created. */
bool interrupt_init(void);

/* Register a discovered fixed source and its firmware-decoded electrical configuration. */
enum interrupt_result interrupt_register_source(const struct hal_interrupt_source* source,
                                                enum hal_interrupt_trigger         trigger,
                                                enum hal_interrupt_polarity polarity, interrupt_source_t* out_token);

/* Claim a fixed source without enabling it.  The returned reference must be released. */
enum interrupt_result interrupt_claim_source(interrupt_source_t source, struct interrupt** out_interrupt);

/* Allocate a message interrupt and return only its device-programmable message values. */
enum interrupt_result interrupt_allocate_message(interrupt_message_context_t context, struct interrupt** out_interrupt,
                                                 struct interrupt_message* out_message);

/* Acquire one reference to an interrupt that has not begun destruction. */
bool interrupt_retain(struct interrupt* interrupt);

/* Release a reference obtained from an interrupt-core operation. */
void interrupt_release(struct interrupt* interrupt);

/* Return public interrupt state without exposing delivery identities. */
enum interrupt_result interrupt_get_info(struct interrupt* interrupt, struct interrupt_info* out_info);

/* Bind an interrupt to exactly one Signal and arm delivery. */
enum interrupt_result interrupt_bind(struct interrupt* interrupt, struct signal* signal);

/* Stop publication and detach the Signal while retaining source ownership. */
enum interrupt_result interrupt_unbind(struct interrupt* interrupt);

/* Permanently disable an interrupt and release its source and delivery identity. */
enum interrupt_result interrupt_destroy(struct interrupt* interrupt);

/* Route one HAL-decoded event to its interrupt object. */
bool interrupt_handle_event(struct hal_interrupt_event event);

/* Rearm the interrupt bound to a Signal after its consumer is ready again. */
void interrupt_signal_ready(signal_id_t signal_id);

/* Detach and mask an Interrupt whose bound Signal has begun destruction. */
bool interrupt_signal_destroying(struct interrupt* interrupt, struct signal* signal);
