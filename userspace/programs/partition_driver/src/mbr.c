#include "mbr.h"

#include <stdio.h>

#define MBR_SIZE 512u
#define MBR_PARTITION_OFFSET 446u
#define MBR_PARTITION_SIZE 16u
#define MBR_SIGNATURE_OFFSET 510u
#define MBR_SIGNATURE_FIRST 0x55u
#define MBR_SIGNATURE_SECOND 0xaau
#define MBR_PARTITION_PROTECTIVE_GPT 0xeeu

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

enum mbr_result mbr_parse(const uint8_t* block, size_t size, uint64_t disk_blocks, struct mbr_table* out_table) {
	if (block == NULL || out_table == NULL || size < MBR_SIZE || disk_blocks == 0u ||
	    block[MBR_SIGNATURE_OFFSET] != MBR_SIGNATURE_FIRST || block[MBR_SIGNATURE_OFFSET + 1u] != MBR_SIGNATURE_SECOND)
		return MBR_RESULT_INVALID;
	*out_table = (struct mbr_table){0};
	for (size_t index = 0u; index < MBR_PARTITION_COUNT; index++) {
		const uint8_t*        encoded = block + MBR_PARTITION_OFFSET + index * MBR_PARTITION_SIZE;
		struct mbr_partition* entry   = &out_table->partitions[index];
		uint8_t               status  = encoded[0];

		entry->bootable    = status == 0x80u;
		entry->type        = encoded[4];
		entry->first_lba   = read_u32_le(encoded + 8u);
		entry->block_count = read_u32_le(encoded + 12u);
		if (status != 0u && status != 0x80u) return MBR_RESULT_INVALID;
		if (entry->type == 0u) {
			if (entry->first_lba != 0u || entry->block_count != 0u) return MBR_RESULT_INVALID;
			continue;
		}
		if (entry->block_count == 0u || entry->first_lba >= disk_blocks ||
		    entry->block_count > disk_blocks - entry->first_lba)
			return MBR_RESULT_INVALID;
		if (entry->type == MBR_PARTITION_PROTECTIVE_GPT) out_table->protective = true;
	}
	return MBR_RESULT_VALID;
}

void mbr_print(const struct mbr_table* table) {
	if (table == NULL) return;
	printf("partition: MBR partition table%s\n", table->protective ? " (protective or hybrid)" : "");
	for (size_t index = 0u; index < MBR_PARTITION_COUNT; index++) {
		const struct mbr_partition* entry = &table->partitions[index];
		if (entry->type == 0u) continue;
		printf("partition: MBR entry %zu type=%02x first_lba=%u blocks=%u bootable=%s\n",
		       index + 1u,
		       entry->type,
		       entry->first_lba,
		       entry->block_count,
		       entry->bootable ? "yes" : "no");
	}
}
