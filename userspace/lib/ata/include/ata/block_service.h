#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/cap.h>

struct ata_buffer;
struct ata_transport;

struct ata_block_info {
	const char* name;
	/* Optional compatible ID placed before the generic block:disk ID. */
	const char* compatible;
	uint64_t block_count;
	uint32_t block_size;
	uint32_t max_transfer_blocks;
	bool writable;
	bool removable;
	bool flush_supported;
};

struct ata_block_backend {
	void* context;
	bool (*read)(void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index,
			uint32_t block_count);
	bool (*write)(void* context, struct ata_buffer* buffer, uint32_t buffer_offset, uint64_t block_index,
			uint32_t block_count);
	bool (*flush)(void* context);
};

/* Publish and serve a block child backed by one ATA-family transport. */
bool ata_block_service_run(cap_id_t device_cap, const struct ata_transport* transport,
		const struct ata_block_info* info, const struct ata_block_backend* backend);
