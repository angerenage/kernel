#pragma once

#include <base/interrupt.h>
#include <base/process.h>
#include <base/signal.h>
#include <core/signal.h>
#include <stdbool.h>

enum interrupt_result {
	INTERRUPT_OK = 0,
	INTERRUPT_INVALID_ARGUMENTS,
	INTERRUPT_ALREADY_ATTACHED,
	INTERRUPT_NOT_FOUND,
	INTERRUPT_NOT_OWNER,
	INTERRUPT_UNAVAILABLE,
	INTERRUPT_NO_MEMORY,
};

/* Attach one exclusive hardware interrupt source to a Signal. */
enum interrupt_result interrupt_attach(process_id_t owner, interrupt_id_t id, struct signal* signal);

/* Detach one source owned by a process. */
enum interrupt_result interrupt_detach(process_id_t owner, interrupt_id_t id);

/* Re-arm the interrupt source attached to one Signal after its notification has been consumed. */
bool interrupt_rearm_signal(signal_id_t signal_id);

/* Publish one kernel-originated notification for an interrupt received by the HAL. */
bool interrupt_dispatch(interrupt_id_t id);

/* Drop every interrupt binding owned by a process being destroyed. */
void interrupt_cleanup_process(process_id_t owner);
