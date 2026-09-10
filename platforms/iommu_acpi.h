#pragma once

#include <core/mm.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IOMMU_ACPI_RSDP_V1_SIZE 20u

struct iommu_acpi_rsdp {
	char     signature[8];
	uint8_t  checksum;
	char     oem_id[6];
	uint8_t  revision;
	uint32_t rsdt_address;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t  extended_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct iommu_acpi_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

static inline const void* iommu_acpi_phys_to_virt(uintptr_t address) {
	return (const void*)(address + boot_info.direct_map_offset);
}

static inline bool iommu_acpi_checksum(const void* data, size_t size) {
	const uint8_t* bytes = data;
	uint8_t        sum   = 0u;
	for (size_t index = 0u; index < size; index++) sum = (uint8_t)(sum + bytes[index]);
	return sum == 0u;
}

static inline bool iommu_acpi_header_valid(const struct iommu_acpi_header* header) {
	return header != NULL && header->length >= sizeof(*header) && iommu_acpi_checksum(header, header->length);
}

static inline const struct iommu_acpi_header* iommu_acpi_table(const char signature[4]) {
	uintptr_t rsdp_address;
	if (!kernel_boot_rsdp_address(&rsdp_address)) return NULL;
	const struct iommu_acpi_rsdp* rsdp = (const struct iommu_acpi_rsdp*)rsdp_address;
	if (memcmp(rsdp->signature, "RSD PTR ", 8u) != 0 || !iommu_acpi_checksum(rsdp, IOMMU_ACPI_RSDP_V1_SIZE))
		return NULL;
	if (rsdp->revision >= 2u && rsdp->length >= sizeof(*rsdp) && iommu_acpi_checksum(rsdp, rsdp->length) &&
	    rsdp->xsdt_address != 0u) {
		const struct iommu_acpi_header* xsdt = iommu_acpi_phys_to_virt((uintptr_t)rsdp->xsdt_address);
		if (!iommu_acpi_header_valid(xsdt) || memcmp(xsdt->signature, "XSDT", 4u) != 0 ||
		    (xsdt->length - sizeof(*xsdt)) % sizeof(uint64_t) != 0u)
			return NULL;
		const uint64_t* entries = (const uint64_t*)((const uint8_t*)xsdt + sizeof(*xsdt));
		for (size_t index = 0u; index < (xsdt->length - sizeof(*xsdt)) / sizeof(*entries); index++) {
			const struct iommu_acpi_header* table = iommu_acpi_phys_to_virt((uintptr_t)entries[index]);
			if (iommu_acpi_header_valid(table) && memcmp(table->signature, signature, 4u) == 0) return table;
		}
		return NULL;
	}
	if (rsdp->rsdt_address == 0u) return NULL;
	const struct iommu_acpi_header* rsdt = iommu_acpi_phys_to_virt((uintptr_t)rsdp->rsdt_address);
	if (!iommu_acpi_header_valid(rsdt) || memcmp(rsdt->signature, "RSDT", 4u) != 0 ||
	    (rsdt->length - sizeof(*rsdt)) % sizeof(uint32_t) != 0u)
		return NULL;
	const uint32_t* entries = (const uint32_t*)((const uint8_t*)rsdt + sizeof(*rsdt));
	for (size_t index = 0u; index < (rsdt->length - sizeof(*rsdt)) / sizeof(*entries); index++) {
		const struct iommu_acpi_header* table = iommu_acpi_phys_to_virt((uintptr_t)entries[index]);
		if (iommu_acpi_header_valid(table) && memcmp(table->signature, signature, 4u) == 0) return table;
	}
	return NULL;
}
