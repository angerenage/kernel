#include "parser.h"

#include <system/capability.h>

#include "tables/mcfg.h"
#include "tables/tpm2.h"

syscall_status_t acpi_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, size_t* out_device_count) {
	size_t           device_count = 0u;
	syscall_status_t status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	if (out_device_count != NULL) *out_device_count = 0u;
	status = acpi_mcfg_parse(provider_cap, root_cap, &device_count);
	if (status != SYSCALL_STATUS_OK) return status;
	status = acpi_tpm2_parse(provider_cap, root_cap, &device_count);
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_device_count != NULL) *out_device_count = device_count;
	return SYSCALL_STATUS_OK;
}
