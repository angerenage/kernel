#pragma once

#include <uacpi/status.h>
#include <uacpi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum uacpi_host_aml_definition_kind {
	UACPI_HOST_AML_DEFINITION_DSDT = 0,
	UACPI_HOST_AML_DEFINITION_SSDT = 1,
};

/*
 * A sanitized ACPI definition block. The AML buffer and this descriptor array
 * must remain valid until uacpi_host_reset() is called: method objects created
 * while loading the namespace retain pointers into the AML buffer.
 *
 * OEM identifiers are fixed-width byte arrays and are not NUL-terminated.
 */
struct uacpi_host_aml_definition {
	uacpi_data_view aml;
	uacpi_u32       oem_revision;
	uacpi_char      oem_id[6];
	uacpi_char      oem_table_id[8];
	uacpi_u8        kind;
	uacpi_u8        revision;
	uacpi_u8        reserved[4];
};

/*
 * Initialize uACPI for an ordered sequence containing one DSDT followed by
 * zero or more SSDTs. This path initializes interpreter and namespace support
 * without discovering, mapping, or reconstructing raw ACPI tables.
 */
uacpi_status uacpi_host_initialize(uacpi_u64 flags, const struct uacpi_host_aml_definition* definitions,
                                   uacpi_size definition_count);

/* Parse and execute the AML buffers supplied to uacpi_host_initialize(). */
uacpi_status uacpi_host_namespace_load(void);

/* Reset uACPI and release references to the caller-owned descriptors. */
void uacpi_host_reset(void);

#ifdef __cplusplus
}
#endif
