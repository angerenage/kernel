#pragma once

#include <base/cap.h>
#include <base/process.h>

/* Register the singleton kernel Interrupts resource capability object. */
bool kernel_capability_interrupts_init(void);

/* Grant the Interrupts resource to one process. */
cap_id_t kernel_capability_interrupts_grant(process_id_t recipient);

/* Return whether the Interrupts resource capability object is available. */
bool kernel_capability_interrupts_available(void);

#if defined(KERNEL_CAPABILITY_INTERRUPT_TEST)
/* Fail the next Interrupts-resource response after its capability has been published. */
void kernel_capability_interrupt_test_fail_next_response(void);

/* Return the capability ID removed by the most recent injected response failure. */
cap_id_t kernel_capability_interrupt_test_last_rollback_cap(void);
#endif
