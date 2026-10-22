#include "parser.h"

#include <system/capability.h>

#include "aml.h"
#include "tables/fadt.h"
#include "tables/mcfg.h"
#include "tables/tpm2.h"

syscall_status_t acpi_parser_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                   cap_id_t dma_cap, cap_id_t interrupts_cap, cap_id_t io_ports_cap,
                                   size_t* out_device_count) {
	struct acpi_device_set devices;
	struct acpi_aml*       aml          = NULL;
	size_t                 device_count = 0u;
	syscall_status_t       status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	(void)dma_cap;
	if (out_device_count != NULL) *out_device_count = 0u;
	status = acpi_device_set_initialize(&devices, root_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = acpi_aml_initialize(provider_cap, &devices, memory_allocator_cap, interrupts_cap, io_ports_cap, &aml);
	if (status == SYSCALL_STATUS_OK) (void)acpi_mcfg_parse(provider_cap, &devices, memory_allocator_cap);
	if (status == SYSCALL_STATUS_OK) (void)acpi_tpm2_parse(provider_cap, &devices, memory_allocator_cap);
	if (status == SYSCALL_STATUS_OK) (void)acpi_fadt_parse(provider_cap, &devices, interrupts_cap, io_ports_cap);
	if (status == SYSCALL_STATUS_OK) status = acpi_aml_add_resources(aml);
	acpi_aml_deinitialize(aml);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_set_commit(&devices, &device_count);
	acpi_device_set_deinitialize(&devices);
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_device_count != NULL) *out_device_count = device_count;
	return SYSCALL_STATUS_OK;
}
