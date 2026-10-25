#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MBR_PARTITION_COUNT 4u

/* A decoded primary MBR partition record. */
struct mbr_partition {
	bool     bootable;
	uint8_t  type;
	uint32_t first_lba;
	uint32_t block_count;
};

/* The four primary records and whether one advertises a GPT. */
struct mbr_table {
	struct mbr_partition partitions[MBR_PARTITION_COUNT];
	bool                 protective;
};

enum mbr_result {
	MBR_RESULT_VALID = 0u,
	MBR_RESULT_INVALID,
};

/* Validate and decode the four primary MBR partition records. */
enum mbr_result mbr_parse(const uint8_t* block, size_t size, uint64_t disk_blocks, struct mbr_table* out_table);

/* Print the populated primary MBR partition records. */
void mbr_print(const struct mbr_table* table);
