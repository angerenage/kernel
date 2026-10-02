#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Operations accepted by the kernel ACPI-table provider. */
enum acpi_provider_op {
	ACPI_PROVIDER_OP_COUNT = 0,
	ACPI_PROVIDER_OP_CLAIM,
};

struct acpi_provider_request_header {
	enum acpi_provider_op op;
};

/* Count all validated, userspace-visible tables carrying signature. */
struct acpi_provider_count_request {
	struct acpi_provider_request_header header;
	char                                signature[4];
};

struct acpi_provider_count_response {
	uint64_t count;
};

/* Exclusively claim the stable zero-based table index for signature. */
struct acpi_provider_claim_request {
	struct acpi_provider_request_header header;
	char                                signature[4];
	uint64_t                            index;
};

struct acpi_provider_claim_response {
	cap_id_t table_cap;
};

/* Operations accepted by one exclusively claimed ACPI-table capability. */
enum acpi_table_op {
	ACPI_TABLE_OP_INFO = 0,
	ACPI_TABLE_OP_READ,
};

struct acpi_table_request_header {
	enum acpi_table_op op;
};

struct acpi_table_info_request {
	struct acpi_table_request_header header;
};

/* Metadata retained after the common ACPI SDT header is removed. */
struct acpi_table_info_response {
	uint64_t body_size;
	uint8_t  revision;
	uint8_t  reserved[7];
};

/* Read an exact range from the headerless table body. */
struct acpi_table_read_request {
	struct acpi_table_request_header header;
	uint32_t                         reserved;
	uint64_t                         offset;
	uint64_t                         size;
};
