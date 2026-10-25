#include "table.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

bool partition_table_append(struct partition_table* table, const struct partition_entry* entry) {
	if (table == NULL || entry == NULL) return false;
	if (table->count == table->capacity) {
		size_t capacity = table->capacity == 0u ? 8u : table->capacity * 2u;
		if (capacity < table->capacity || capacity > SIZE_MAX / sizeof(*table->entries)) return false;
		struct partition_entry* entries = realloc(table->entries, capacity * sizeof(*entries));
		if (entries == NULL) return false;
		table->entries  = entries;
		table->capacity = capacity;
	}
	table->entries[table->count++] = *entry;
	return true;
}

void partition_table_destroy(struct partition_table* table) {
	if (table == NULL) return;
	free(table->entries);
	*table = (struct partition_table){0};
}

void partition_format_guid(const uint8_t guid[16], char output[PARTITION_GUID_STRING_SIZE]) {
	if (guid == NULL || output == NULL) return;
	(void)snprintf(output,
	               PARTITION_GUID_STRING_SIZE,
	               "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	               read_u32_le(guid),
	               (unsigned)((uint16_t)guid[4] | (uint16_t)guid[5] << 8u),
	               (unsigned)((uint16_t)guid[6] | (uint16_t)guid[7] << 8u),
	               guid[8],
	               guid[9],
	               guid[10],
	               guid[11],
	               guid[12],
	               guid[13],
	               guid[14],
	               guid[15]);
}
