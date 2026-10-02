#pragma once

#include <base/cap.h>
#include <stdbool.h>

/* Create the ACPI provider when validated ACPI tables are available. */
bool kernel_capability_acpi_init(void);

/* Return whether the ACPI provider can currently be granted. */
bool kernel_capability_acpi_available(void);

/* Grant authority to inspect and exclusively claim userspace-visible ACPI tables. */
cap_id_t kernel_capability_acpi_grant(process_id_t recipient);

#if defined(KERNEL_CAPABILITY_ACPI_TEST)
void     kernel_capability_acpi_test_fail_next_claim_response(void);
cap_id_t kernel_capability_acpi_test_last_rollback_cap(void);
#endif
