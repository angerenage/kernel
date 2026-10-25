#include "partition.h"

#include <runtime/block.h>
#include <stdint.h>
#include <stdio.h>
#include <system/time.h>

#include "gpt.h"
#include "mbr.h"

static bool probe_partition_table(struct block_device* device) {
	struct block_buffer buffer = {
		.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID};
	struct mbr_table table;
	bool             success = false;
	enum gpt_result  gpt     = gpt_probe(device);

	if (gpt == GPT_RESULT_VALID) return true;
	if (gpt == GPT_RESULT_IO_ERROR) return false;

	if (block_buffer_allocate(device, device->block_size, &buffer) != SYSCALL_STATUS_OK) goto done;
	if (block_read(device, &buffer, 0u, 0u, 1u) != SYSCALL_STATUS_OK) goto done;
	if (mbr_parse(buffer.address, device->block_size, device->block_count, &table) != MBR_RESULT_VALID) {
		printf("partition: no valid MBR or GPT partition table found\n");
		success = true;
		goto done;
	}
	if (table.protective) printf("partition: protective MBR has no valid GPT; falling back to MBR\n");
	mbr_print(&table);
	success = true;

done:
	block_buffer_release(device, &buffer);
	return success;
}

bool partition_driver_run(cap_id_t device_cap) {
	struct block_device device  = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	bool                success = block_device_open(device_cap, &device) == SYSCALL_STATUS_OK;

	if (success)
		printf("partition: probing disk with %llu blocks of %u bytes (max transfer %u blocks, writable=%s)\n",
		       (unsigned long long)device.block_count,
		       device.block_size,
		       device.max_transfer_blocks,
		       (device.flags & BLOCK_DEVICE_WRITABLE) != 0u ? "yes" : "no");
	if (success) success = probe_partition_table(&device);
	while (success) success = sleep_ms(1000u) == SYSCALL_STATUS_OK;
	block_device_close(&device);
	return success;
}
