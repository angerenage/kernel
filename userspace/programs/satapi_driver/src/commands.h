#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ata_buffer;
struct ata_transport;

enum satapi_media_status {
	SATAPI_MEDIA_READY = 0u,
	SATAPI_MEDIA_NOT_PRESENT,
	SATAPI_MEDIA_NOT_READY,
	SATAPI_MEDIA_ERROR,
};

/* Check whether removable media is ready without attempting a media data command. */
enum satapi_media_status satapi_test_media(const struct ata_transport* transport);

/* Retrieve the total number of blocks and the size of each block on a SATAPI device. */
bool satapi_read_capacity(const struct ata_transport* transport, uint64_t* out_block_count, uint32_t* out_block_size);

/* Read one or more contiguous blocks from a SATAPI device. */
bool satapi_read_blocks(const struct ata_transport* transport, struct ata_buffer* buffer, uint32_t buffer_offset,
		uint32_t lba, uint32_t block_count, uint32_t block_size);
