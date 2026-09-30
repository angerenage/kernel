#include "tpm.h"

#include <base/device.h>
#include <base/hardware/tpm.h>
#include <firmware/acpi.h>
#include <firmware/dt/device.h>
#include <kernel/device.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TPM_DT_TIS_MMIO_COMPATIBLE "tcg,tpm-tis-mmio"
#define TPM_FIFO_DEFAULT_ADDRESS 0xfed40000ull
#define TPM_FIFO_REGISTER_SIZE 0x5000ull
#define TPM2_ACPI_START_METHOD_FIFO_MMIO 6u
#define TPM2_ACPI_START_METHOD_CRB 7u

struct acpi_tpm2 {
	struct acpi_sdt_header header;
	uint16_t               platform_class;
	uint16_t               reserved;
	uint64_t               control_address;
	uint32_t               start_method;
} __attribute__((packed));

static bool tpm_acpi_device(const struct acpi_tpm2* table, struct tpm_device* out_device) {
	struct tpm_device device;
	uint64_t          control_address;
	uint32_t          start_method;
	uint32_t          length;
	uint16_t          platform_class;
	uint16_t          reserved;

	if (table == NULL || out_device == NULL || table->header.revision < 4u) return false;
	memcpy(&length, &table->header.length, sizeof(length));
	if (length < sizeof(*table)) return false;
	memcpy(&platform_class, &table->platform_class, sizeof(platform_class));
	memcpy(&reserved, &table->reserved, sizeof(reserved));
	memcpy(&control_address, &table->control_address, sizeof(control_address));
	memcpy(&start_method, &table->start_method, sizeof(start_method));
	if (platform_class > 1u || reserved != 0u) return false;

	memset(&device, 0, sizeof(device));
	device.family = TPM_FAMILY_2_0;
	switch (start_method) {
	case TPM2_ACPI_START_METHOD_FIFO_MMIO:
		if (control_address == 0u) control_address = TPM_FIFO_DEFAULT_ADDRESS;
		if (control_address > UINT64_MAX - TPM_FIFO_REGISTER_SIZE) return false;
		device.interface                         = TPM_INTERFACE_FIFO_MMIO;
		device.access.fifo_mmio.register_address = control_address;
		device.access.fifo_mmio.register_size    = TPM_FIFO_REGISTER_SIZE;
		break;
	case TPM2_ACPI_START_METHOD_CRB:
		if (control_address == 0u) return false;
		device.interface                       = TPM_INTERFACE_CRB;
		device.access.crb.control_area_address = control_address;
		break;
	default:
		return false;
	}
	*out_device = device;
	return true;
}

static bool tpm_acpi_find(size_t target, struct tpm_device* out_device, size_t* out_count) {
	acpi_cursor_t cursor = ACPI_CURSOR_INIT;
	size_t        count  = 0u;

	for (;;) {
		const struct acpi_tpm2* table = (const struct acpi_tpm2*)acpi_table_next("TPM2", &cursor);
		struct tpm_device       device;

		if (table == NULL) break;
		if (!tpm_acpi_device(table, &device)) continue;
		if (count == target && out_device != NULL) {
			*out_device = device;
			return true;
		}
		if (count == SIZE_MAX) break;
		count++;
	}
	if (out_count != NULL) *out_count = count;
	return false;
}

static bool tpm_dt_device(struct dt_node node, struct tpm_device* out_device) {
	struct tpm_device device;
	struct dt_reg     reg;

	if (out_device == NULL || !dt_node_reg(node, 0u, &reg) || reg.size < TPM_FIFO_REGISTER_SIZE ||
	    reg.address > UINT64_MAX - reg.size)
		return false;
	memset(&device, 0, sizeof(device));
	device.family                            = TPM_FAMILY_UNSPECIFIED;
	device.interface                         = TPM_INTERFACE_FIFO_MMIO;
	device.access.fifo_mmio.register_address = reg.address;
	device.access.fifo_mmio.register_size    = reg.size;
	*out_device                              = device;
	return true;
}

static bool tpm_dt_find(size_t target, struct tpm_device* out_device, size_t* out_count) {
	size_t devices = dt_device_count(TPM_DT_TIS_MMIO_COMPATIBLE);
	size_t count   = 0u;

	for (size_t index = 0u; index < devices; index++) {
		struct tpm_device device;

		if (!tpm_dt_device(dt_device_at(TPM_DT_TIS_MMIO_COMPATIBLE, index), &device)) continue;
		if (count == target && out_device != NULL) {
			*out_device = device;
			return true;
		}
		if (count == SIZE_MAX) break;
		count++;
	}
	if (out_count != NULL) *out_count = count;
	return false;
}

static size_t tpm_candidate_count(void) {
	size_t acpi_count = 0u;
	size_t dt_count   = 0u;

	(void)tpm_acpi_find(SIZE_MAX, NULL, &acpi_count);
	(void)tpm_dt_find(SIZE_MAX, NULL, &dt_count);
	return acpi_count > SIZE_MAX - dt_count ? SIZE_MAX : acpi_count + dt_count;
}

static bool tpm_candidate_at(size_t index, struct tpm_device* out_device) {
	size_t acpi_count = 0u;

	if (out_device == NULL) return false;
	if (tpm_acpi_find(index, out_device, &acpi_count)) return true;
	return index >= acpi_count && tpm_dt_find(index - acpi_count, out_device, NULL);
}

static bool tpm_same_access(const struct tpm_device* first, const struct tpm_device* second) {
	if (first == NULL || second == NULL || first->interface != second->interface) return false;
	switch (first->interface) {
	case TPM_INTERFACE_FIFO_MMIO:
		return first->access.fifo_mmio.register_address == second->access.fifo_mmio.register_address;
	case TPM_INTERFACE_CRB:
		return first->access.crb.control_area_address == second->access.crb.control_area_address;
	default:
		return false;
	}
}

static bool tpm_candidate_is_first(size_t index, const struct tpm_device* candidate) {
	for (size_t previous_index = 0u; previous_index < index; previous_index++) {
		struct tpm_device previous;

		if (!tpm_candidate_at(previous_index, &previous)) return false;
		if (tpm_same_access(&previous, candidate)) return false;
	}
	return true;
}

bool kernel_device_register_tpms(void) {
	size_t candidates = tpm_candidate_count();

	if (!kernel_device_register_type(KERNEL_DEVICE_TYPE_TPM, sizeof(struct tpm_device))) return false;
	for (size_t index = 0u; index < candidates; index++) {
		struct tpm_device device;

		if (!tpm_candidate_at(index, &device)) return false;
		if (!tpm_candidate_is_first(index, &device)) continue;
		if (!kernel_device_register(KERNEL_DEVICE_TYPE_TPM, &device, sizeof(device))) return false;
	}
	return true;
}
