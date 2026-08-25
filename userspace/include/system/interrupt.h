#pragma once

#include <base/cap.h>
#include <base/interrupt.h>
#include <base/syscall.h>

/* Attach one exclusive platform interrupt source to a Signal capability. */
syscall_status_t interrupt_attach(interrupt_id_t id, cap_id_t signal_cap);

/* Stop delivering one source owned by the caller. */
syscall_status_t interrupt_detach(interrupt_id_t id);
