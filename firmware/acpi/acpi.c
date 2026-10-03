#include <firmware/acpi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ACPI_RSDP_V1_SIZE 20u
#define ACPI_RSDP_V2_MIN_SIZE 36u
#define ACPI_RSDP_MAX_SIZE 4096u
#define ACPI_SDT_MAX_SIZE (16u * 1024u * 1024u)
#define ACPI_FADT_DSDT_OFFSET 40u
#define ACPI_FADT_X_DSDT_OFFSET 140u

struct acpi_root_state {
	const struct acpi_sdt_header* table;
	size_t                        entry_count;
	size_t                        entry_size;
};

static struct acpi_root_state        acpi_root;
static const struct acpi_sdt_header* acpi_dsdt_table;
static bool                          acpi_initialized;
static const struct boot_info*       acpi_boot_info;

static uint32_t acpi_read_u32(const uint8_t* value) {
	uint32_t result;

	memcpy(&result, value, sizeof(result));
	return result;
}

static uint64_t acpi_read_u64(const uint8_t* value) {
	uint64_t result;

	memcpy(&result, value, sizeof(result));
	return result;
}

static bool acpi_signature_valid(const char signature[4], const struct acpi_sdt_header* table) {
	if (signature == NULL || table == NULL) return false;
	return memcmp(signature, table->signature, sizeof(table->signature)) == 0;
}

static bool acpi_checksum_valid(const void* data, size_t size) {
	const uint8_t* bytes    = data;
	uint8_t        checksum = 0u;

	if (data == NULL || size == 0u) return false;
	for (size_t index = 0u; index < size; index++) checksum = (uint8_t)(checksum + bytes[index]);
	return checksum == 0u;
}

static bool acpi_physical_span(uintptr_t physical, size_t size, const void** out) {
	const struct mem_range* ranges;
	size_t                  range_count;
	uintptr_t               end;
	uintptr_t               cursor;
	uintptr_t               virtual;

	if (size == 0u || size > UINTPTR_MAX - physical || acpi_boot_info == NULL) return false;
	end         = physical + size;
	ranges      = acpi_boot_info->memory_map;
	range_count = acpi_boot_info->memory_map_count;
	if (ranges == NULL || range_count == 0u) return false;

	cursor = physical;
	while (cursor < end) {
		uintptr_t covered_end = cursor;

		for (size_t index = 0u; index < range_count; index++) {
			uintptr_t range_end;

			if ((ranges[index].type != MEM_RANGE_ACPI && ranges[index].type != MEM_RANGE_RESERVED) ||
			    ranges[index].length == 0u || ranges[index].length > UINTPTR_MAX - ranges[index].base)
				continue;
			range_end = ranges[index].base + ranges[index].length;
			if (ranges[index].base <= cursor && range_end > covered_end) covered_end = range_end;
		}
		if (covered_end == cursor) return false;
		cursor = covered_end < end ? covered_end : end;
	}

	if (physical > UINTPTR_MAX - acpi_boot_info->direct_map_offset) return false;
	virtual = physical + acpi_boot_info->direct_map_offset;
	if (size > UINTPTR_MAX - virtual) return false;
	if (out != NULL) *out = (const void*)virtual;
	return true;
}

static bool acpi_sdt(uintptr_t physical, const char signature[4], const struct acpi_sdt_header** out) {
	const struct acpi_sdt_header* header;
	const void*                   bytes;
	uint32_t                      length;

	if (!acpi_physical_span(physical, sizeof(*header), &bytes)) return false;
	header = bytes;
	if (!acpi_signature_valid(signature, header)) return false;
	memcpy(&length, &header->length, sizeof(length));
	if (length < sizeof(*header) || length > ACPI_SDT_MAX_SIZE || !acpi_physical_span(physical, (size_t)length, &bytes))
		return false;
	header = bytes;
	if (!acpi_checksum_valid(header, (size_t)length)) return false;
	if (out != NULL) *out = header;
	return true;
}

static const struct acpi_sdt_header* acpi_root_table_next(const struct acpi_root_state* root, const char signature[4],
                                                          acpi_cursor_t* cursor) {
	acpi_cursor_t index = cursor == NULL ? ACPI_CURSOR_INIT : *cursor;

	if (index > root->entry_count) index = root->entry_count;
	for (; index < root->entry_count; index++) {
		const uint8_t* entry = (const uint8_t*)root->table + sizeof(*root->table) + index * root->entry_size;
		const struct acpi_sdt_header* table;
		uint64_t                      physical;

		physical = root->entry_size == sizeof(uint64_t) ? acpi_read_u64(entry) : acpi_read_u32(entry);
		if (physical == 0u || physical > UINTPTR_MAX || !acpi_sdt((uintptr_t)physical, signature, &table)) continue;
		if (cursor != NULL) *cursor = index + 1u;
		return table;
	}
	if (cursor != NULL) *cursor = root->entry_count;
	return NULL;
}

static const struct acpi_sdt_header* acpi_fadt_dsdt(const struct acpi_root_state* root) {
	const struct acpi_sdt_header* fadt = acpi_root_table_next(root, "FACP", NULL);
	const struct acpi_sdt_header* dsdt;
	const uint8_t*                bytes;
	uint64_t                      physical;
	uint32_t                      length;

	if (fadt == NULL) return NULL;
	bytes = (const uint8_t*)fadt;
	memcpy(&length, &fadt->length, sizeof(length));
	if (length >= ACPI_FADT_X_DSDT_OFFSET + sizeof(uint64_t) &&
	    (physical = acpi_read_u64(bytes + ACPI_FADT_X_DSDT_OFFSET)) != 0u) {
		if (physical > UINTPTR_MAX) return NULL;
	}
	else {
		if (length < ACPI_FADT_DSDT_OFFSET + sizeof(uint32_t)) return NULL;
		physical = acpi_read_u32(bytes + ACPI_FADT_DSDT_OFFSET);
	}
	if (physical == 0u) return NULL;
	return acpi_sdt((uintptr_t)physical, "DSDT", &dsdt) ? dsdt : NULL;
}

bool acpi_init(const struct boot_info* info) {
	const struct acpi_sdt_header* root;
	const uint8_t*                bytes;
	const void*                   span;
	uintptr_t                     rsdp_virtual;
	uintptr_t                     rsdp_physical;
	uintptr_t                     root_physical;
	size_t                        rsdp_size = ACPI_RSDP_V1_SIZE;
	size_t                        entry_size;
	uint32_t                      root_length;
	struct acpi_root_state        root_state;

	const void* rsdp;
	if (__atomic_load_n(&acpi_initialized, __ATOMIC_ACQUIRE)) return true;
	if (info == NULL || info->rsdp_address == 0u) return false;
	acpi_boot_info = info;
	rsdp           = (const void*)info->rsdp_address;
	rsdp_virtual   = (uintptr_t)rsdp;
	if (rsdp_virtual < info->direct_map_offset) return false;
	rsdp_physical = rsdp_virtual - info->direct_map_offset;
	if (!acpi_physical_span(rsdp_physical, ACPI_RSDP_V1_SIZE, &span) || span != rsdp) return false;
	bytes = span;
	if (memcmp(bytes, "RSD PTR ", 8u) != 0 || !acpi_checksum_valid(bytes, ACPI_RSDP_V1_SIZE)) return false;

	if (bytes[15] >= 2u) {
		if (!acpi_physical_span(rsdp_physical, 24u, &span) || span != rsdp) return false;
		bytes     = span;
		rsdp_size = (size_t)acpi_read_u32(bytes + 20u);
		if (rsdp_size < ACPI_RSDP_V2_MIN_SIZE || rsdp_size > ACPI_RSDP_MAX_SIZE ||
		    !acpi_physical_span(rsdp_physical, rsdp_size, &span) || span != rsdp ||
		    !acpi_checksum_valid(span, rsdp_size))
			return false;
		bytes = span;
	}

	if (bytes[15] >= 2u && acpi_read_u64(bytes + 24u) != 0u) {
		uint64_t xsdt = acpi_read_u64(bytes + 24u);

		if (xsdt > UINTPTR_MAX) return false;
		root_physical = (uintptr_t)xsdt;
		entry_size    = sizeof(uint64_t);
		if (!acpi_sdt(root_physical, "XSDT", &root)) return false;
	}
	else {
		root_physical = (uintptr_t)acpi_read_u32(bytes + 16u);
		entry_size    = sizeof(uint32_t);
		if (root_physical == 0u || !acpi_sdt(root_physical, "RSDT", &root)) return false;
	}

	memcpy(&root_length, &root->length, sizeof(root_length));
	if (((size_t)root_length - sizeof(*root)) % entry_size != 0u) return false;
	root_state = (struct acpi_root_state){
		.table       = root,
		.entry_count = ((size_t)root_length - sizeof(*root)) / entry_size,
		.entry_size  = entry_size,
	};
	acpi_dsdt_table = acpi_fadt_dsdt(&root_state);
	acpi_root       = root_state;
	__atomic_store_n(&acpi_initialized, true, __ATOMIC_RELEASE);
	return true;
}

bool acpi_available(void) {
	return __atomic_load_n(&acpi_initialized, __ATOMIC_ACQUIRE);
}

const struct acpi_sdt_header* acpi_table_next(const char signature[4], acpi_cursor_t* cursor) {
	if (signature == NULL || !__atomic_load_n(&acpi_initialized, __ATOMIC_ACQUIRE) ||
	    memcmp(signature, "RSDT", 4u) == 0 || memcmp(signature, "XSDT", 4u) == 0)
		return NULL;
	return acpi_root_table_next(&acpi_root, signature, cursor);
}

const struct acpi_sdt_header* acpi_dsdt(void) {
	if (!__atomic_load_n(&acpi_initialized, __ATOMIC_ACQUIRE)) return NULL;
	return acpi_dsdt_table;
}
