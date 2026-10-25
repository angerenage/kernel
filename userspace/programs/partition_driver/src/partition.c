#include "partition.h"

#include <runtime/block.h>
#include <stdint.h>
#include <stdio.h>

#include "gpt.h"
#include "mbr.h"
#include "service.h"
#include "table.h"

static bool append_mbr_partitions(const struct mbr_table* mbr, struct partition_table* table) {
	for (size_t index = 0u; index < MBR_PARTITION_COUNT; index++) {
		const struct mbr_partition* source = &mbr->partitions[index];
		if (source->type == 0u) continue;
		struct partition_entry entry = {.scheme       = PARTITION_SCHEME_MBR,
		                                .table_index  = (uint32_t)index + 1u,
		                                .first_lba    = source->first_lba,
		                                .block_count  = source->block_count,
		                                .mbr.type     = source->type,
		                                .mbr.bootable = source->bootable};
		if (!partition_table_append(table, &entry)) return false;
	}
	return true;
}

enum partition_probe_result {
	PARTITION_PROBE_FOUND = 0u,
	PARTITION_PROBE_EMPTY,
	PARTITION_PROBE_ERROR,
};

static enum partition_probe_result probe_partition_table(struct block_device*    device,
                                                         struct partition_table* partitions) {
	struct block_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	struct mbr_table table;
	enum gpt_result  gpt = gpt_probe(device, partitions);

	if (gpt == GPT_RESULT_VALID) return partitions->count == 0u ? PARTITION_PROBE_EMPTY : PARTITION_PROBE_FOUND;
	if (gpt == GPT_RESULT_IO_ERROR || gpt == GPT_RESULT_NO_MEMORY) return PARTITION_PROBE_ERROR;

	if (block_buffer_allocate(device, device->block_size, &buffer) != SYSCALL_STATUS_OK) goto done;
	if (block_read(device, &buffer, 0u, 0u, 1u) != SYSCALL_STATUS_OK) goto done;
	if (mbr_parse(buffer.address, device->block_size, device->block_count, &table) != MBR_RESULT_VALID) {
		printf("partition: no valid MBR or GPT partition table found\n");
		block_buffer_release(device, &buffer);
		return PARTITION_PROBE_EMPTY;
	}
	if (table.protective) printf("partition: protective MBR has no valid GPT; falling back to MBR\n");
	if (!append_mbr_partitions(&table, partitions)) goto done;
	mbr_print(&table);
	block_buffer_release(device, &buffer);
	return partitions->count == 0u ? PARTITION_PROBE_EMPTY : PARTITION_PROBE_FOUND;

done:
	block_buffer_release(device, &buffer);
	return PARTITION_PROBE_ERROR;
}

enum device_driver_result partition_driver_run(cap_id_t device_cap) {
	struct block_device       device     = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	struct partition_table    partitions = {0};
	enum device_driver_result result     = DEVICE_DRIVER_ERROR;
	bool                      success    = block_device_open(device_cap, &device) == SYSCALL_STATUS_OK;

	if (success)
		printf("partition: probing disk with %llu blocks of %u bytes (max transfer %u blocks, writable=%s)\n",
		       (unsigned long long)device.block_count,
		       device.block_size,
		       device.max_transfer_blocks,
		       (device.flags & BLOCK_DEVICE_WRITABLE) != 0u ? "yes" : "no");
	if (success) {
		enum partition_probe_result probe = probe_partition_table(&device, &partitions);
		if (probe == PARTITION_PROBE_EMPTY) result = DEVICE_DRIVER_UNSUPPORTED;
		else if (probe == PARTITION_PROBE_FOUND) (void)partition_service_run(device_cap, &device, &partitions);
	}
	partition_table_destroy(&partitions);
	block_device_close(&device);
	return result;
}
