#include "tpm2.h"

#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <system/acpi.h>
#include <system/capability.h>

#include "../device.h"

#define ACPI_TPM2_BODY_SIZE 16u
#define ACPI_TPM2_MIN_REVISION 4u
#define TPM_FIFO_DEFAULT_ADDRESS 0xfed40000ull
#define TPM_FIFO_REGISTER_SIZE 0x5000ull
#define TPM2_START_METHOD_FIFO_MMIO 6u
#define TPM2_START_METHOD_CRB 7u

/* Interface-specific IDs precede the common TPM2 contract used by multi-interface drivers. */
#define TPM_COMPATIBLE_FIFO_MMIO "acpi:tpm2_fifo_mmio"
#define TPM_COMPATIBLE_CRB "acpi:tpm2_crb"
#define TPM_COMPATIBLE_GENERIC "acpi:tpm2"

#define TPM_PROPERTY_FAMILY "family"
#define TPM_PROPERTY_INTERFACE "interface"
#define TPM_PROPERTY_CONTROL_AREA_ADDRESS "control_area_address"
#define TPM_RESOURCE_REGISTERS "registers"

struct acpi_tpm2_device {
	uint64_t control_address;
	uint32_t start_method;
};

static uint16_t read_u16_le(const uint8_t value[2]) {
	return (uint16_t)value[0] | ((uint16_t)value[1] << 8u);
}

static uint32_t read_u32_le(const uint8_t value[4]) {
	return (uint32_t)value[0] | ((uint32_t)value[1] << 8u) | ((uint32_t)value[2] << 16u) | ((uint32_t)value[3] << 24u);
}

static uint64_t read_u64_le(const uint8_t value[8]) {
	uint64_t result = 0u;

	for (size_t index = 0u; index < 8u; index++) result |= (uint64_t)value[index] << (index * 8u);
	return result;
}

static bool tpm2_device_decode(const struct acpi_table_info_response* info, const uint8_t body[ACPI_TPM2_BODY_SIZE],
                               struct acpi_tpm2_device* out_device) {
	uint64_t control_address;
	uint32_t start_method;

	if (info == NULL || body == NULL || out_device == NULL || info->revision < ACPI_TPM2_MIN_REVISION ||
	    info->body_size < ACPI_TPM2_BODY_SIZE || read_u16_le(body) > 1u || read_u16_le(body + 2u) != 0u)
		return false;
	control_address = read_u64_le(body + 4u);
	start_method    = read_u32_le(body + 12u);
	switch (start_method) {
	case TPM2_START_METHOD_FIFO_MMIO:
		if (control_address == 0u) control_address = TPM_FIFO_DEFAULT_ADDRESS;
		if (control_address > UINT64_MAX - TPM_FIFO_REGISTER_SIZE) return false;
		break;
	case TPM2_START_METHOD_CRB:
		if (control_address == 0u) return false;
		break;
	default:
		return false;
	}
	*out_device = (struct acpi_tpm2_device){
		.control_address = control_address,
		.start_method    = start_method,
	};
	return true;
}

static syscall_status_t submit_tpm2_device(cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                           const struct acpi_tpm2_device* device) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	const char*           compatible;
	const char*           interface;
	syscall_status_t      status;

	if (device->start_method == TPM2_START_METHOD_FIFO_MMIO) {
		compatible = TPM_COMPATIBLE_FIFO_MMIO;
		interface  = "fifo_mmio";
	}
	else {
		compatible = TPM_COMPATIBLE_CRB;
		interface  = "crb";
	}
	status = device_builder_begin_root(root_cap, &builder);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_compatible(&builder, compatible);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_compatible(&builder, TPM_COMPATIBLE_GENERIC);
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_string(&builder, TPM_PROPERTY_FAMILY, "2.0");
	if (status == SYSCALL_STATUS_OK) status = acpi_device_add_string(&builder, TPM_PROPERTY_INTERFACE, interface);
	if (status == SYSCALL_STATUS_OK && device->start_method == TPM2_START_METHOD_FIFO_MMIO)
		status = device_builder_add_mmio_resource(&builder,
		                                          memory_allocator_cap,
		                                          TPM_RESOURCE_REGISTERS,
		                                          sizeof(TPM_RESOURCE_REGISTERS) - 1u,
		                                          (uintptr_t)device->control_address,
		                                          TPM_FIFO_REGISTER_SIZE);
	if (status == SYSCALL_STATUS_OK && device->start_method == TPM2_START_METHOD_CRB)
		status = acpi_device_add_u64(&builder, TPM_PROPERTY_CONTROL_AREA_ADDRESS, device->control_address);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status;
}

static syscall_status_t parse_tpm2_table(cap_id_t table_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                         size_t* device_count) {
	struct acpi_table_info_response info;
	struct acpi_tpm2_device         device;
	uint8_t                         body[ACPI_TPM2_BODY_SIZE];
	syscall_status_t                status;

	status = acpi_table_info(table_cap, &info);
	if (status != SYSCALL_STATUS_OK) return status;
	if (info.revision < ACPI_TPM2_MIN_REVISION || info.body_size < sizeof(body)) return SYSCALL_STATUS_OK;
	status = acpi_table_read(table_cap, 0u, body, sizeof(body));
	if (status != SYSCALL_STATUS_OK) return status;
	if (!tpm2_device_decode(&info, body, &device)) return SYSCALL_STATUS_OK;
	if (*device_count == SIZE_MAX) return SYSCALL_STATUS_FAILED;
	status = submit_tpm2_device(root_cap, memory_allocator_cap, &device);
	if (status != SYSCALL_STATUS_OK) return status;
	(*device_count)++;
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_tpm2_parse(cap_id_t provider_cap, cap_id_t root_cap, cap_id_t memory_allocator_cap,
                                 size_t* device_count) {
	uint64_t         table_count;
	syscall_status_t status;

	if (provider_cap == CAP_ID_INVALID || root_cap == CAP_ID_INVALID || memory_allocator_cap == CAP_ID_INVALID ||
	    device_count == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = acpi_table_count(provider_cap, "TPM2", &table_count);
	if (status != SYSCALL_STATUS_OK) return status;
	for (uint64_t index = 0u; index < table_count; index++) {
		cap_id_t table_cap = CAP_ID_INVALID;

		status = acpi_table_claim(provider_cap, "TPM2", index, &table_cap);
		if (status == SYSCALL_STATUS_OK)
			status = parse_tpm2_table(table_cap, root_cap, memory_allocator_cap, device_count);
		if (table_cap != CAP_ID_INVALID) {
			syscall_status_t drop_status = cap_drop(table_cap);
			if (status == SYSCALL_STATUS_OK) status = drop_status;
		}
		if (status != SYSCALL_STATUS_OK) return status;
	}
	return SYSCALL_STATUS_OK;
}
