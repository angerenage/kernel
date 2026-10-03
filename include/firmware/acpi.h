#pragma once

#include <boot/info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Root-table position used by allocation-free ACPI table iteration. */
typedef size_t acpi_cursor_t;

/* Initial cursor value for a scan beginning at the first root-table entry. */
#define ACPI_CURSOR_INIT ((acpi_cursor_t)0u)

/* Common header present at the beginning of every ACPI system description table. */
struct acpi_sdt_header {
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

/* Validate the RSDP described by boot info and publish its authoritative root table once. */
bool acpi_init(const struct boot_info* info);

/* Return whether a validated ACPI root table has been published. */
bool acpi_available(void);

/* Return the next validated table matching signature, optionally advancing cursor. */
const struct acpi_sdt_header* acpi_table_next(const char signature[4], acpi_cursor_t* cursor);

/* Return the validated DSDT referenced by the FADT, if available. */
const struct acpi_sdt_header* acpi_dsdt(void);
