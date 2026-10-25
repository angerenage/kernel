#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PARTITION_GPT_NAME_SIZE 37u
#define PARTITION_GUID_STRING_SIZE 37u

enum partition_scheme {
	PARTITION_SCHEME_GPT = 0u,
	PARTITION_SCHEME_MBR,
};

/* Metadata required to publish one bounded block-device view. */
struct partition_entry {
	enum partition_scheme scheme;
	uint32_t              table_index;
	uint64_t              first_lba;
	uint64_t              block_count;
	union {
		struct {
			uint8_t  type_guid[16];
			uint8_t  unique_guid[16];
			uint64_t attributes;
			char     name[PARTITION_GPT_NAME_SIZE];
		} gpt;
		struct {
			uint8_t type;
			bool    bootable;
		} mbr;
	};
};

/* Dynamically sized collection of populated partition records. */
struct partition_table {
	struct partition_entry* entries;
	size_t                  count;
	size_t                  capacity;
};

/* Append one partition record without changing existing entries on failure. */
bool partition_table_append(struct partition_table* table, const struct partition_entry* entry);

/* Release every allocation owned by a partition table. */
void partition_table_destroy(struct partition_table* table);

/* Format one GPT GUID using its canonical mixed-endian textual representation. */
void partition_format_guid(const uint8_t guid[16], char output[PARTITION_GUID_STRING_SIZE]);
