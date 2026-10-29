#include "service.h"

#include <base/channel.h>
#include <base/signal.h>
#include <protocol/device.h>
#include <runtime/block.h>
#include <runtime/device.h>
#include <runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/signal.h>

#include "table.h"

static cap_rights_t block_rights(const struct block_device* parent) {
	cap_rights_t rights = CAP_CALL | CAP_READ;
	if ((parent->flags & BLOCK_DEVICE_WRITABLE) != 0u) rights |= CAP_WRITE;
	return rights;
}

static syscall_status_t add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t encoded[sizeof(value)];

	for (size_t index = 0u; index < sizeof(encoded); index++) encoded[index] = (uint8_t)(value >> (index * 8u));
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, encoded, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static syscall_status_t add_bool(const struct device_builder* builder, const char* name, bool value) {
	uint8_t          encoded = value ? 1u : 0u;
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_BOOLEAN, 1u, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_append_property(builder, 0u, &encoded, sizeof(encoded));
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static syscall_status_t add_string(const struct device_builder* builder, const char* name, const char* value) {
	size_t           size = strlen(value);
	syscall_status_t status =
		device_builder_begin_property(builder, name, strlen(name), DEVICE_PROPERTY_UTF8_STRING, 1u, size);
	if (status == SYSCALL_STATUS_OK && size != 0u) status = device_builder_append_property(builder, 0u, value, size);
	if (status == SYSCALL_STATUS_OK) status = device_builder_finish_property(builder);
	return status;
}

static bool create_child(cap_id_t parent, cap_id_t block_cap, const struct block_device* device,
                         const struct partition_entry* partition) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char                  name[32];
	char                  compatible[5u + PARTITION_GUID_STRING_SIZE];
	char                  type_guid[PARTITION_GUID_STRING_SIZE];
	char                  unique_guid[PARTITION_GUID_STRING_SIZE];
	int                   length;

	length = snprintf(name, sizeof(name), "partition-%u", partition->table_index);
	if (length <= 0 || (size_t)length >= sizeof(name)) return false;
	if (partition->scheme == PARTITION_SCHEME_GPT) {
		partition_format_guid(partition->gpt.type_guid, type_guid);
		partition_format_guid(partition->gpt.unique_guid, unique_guid);
		length = snprintf(compatible, sizeof(compatible), "gpt:%s", type_guid);
	}
	else {
		length = snprintf(compatible, sizeof(compatible), "mbr:%02x", partition->mbr.type);
	}
	if (length <= 0 || (size_t)length >= sizeof(compatible)) return false;

	syscall_status_t status = device_builder_begin_child(parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, strlen(name));
	if (status == SYSCALL_STATUS_OK) status = device_builder_add_compatible(&builder, compatible, strlen(compatible));
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(
			&builder, BLOCK_COMPATIBLE_PARTITION, sizeof(BLOCK_COMPATIBLE_PARTITION) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, BLOCK_COMPATIBLE_DEVICE, sizeof(BLOCK_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "partition_index", partition->table_index);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "first_lba", partition->first_lba);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_count", partition->block_count);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "block_size", device->block_size);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "max_transfer_blocks", device->max_transfer_blocks);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "writable", (device->flags & BLOCK_DEVICE_WRITABLE) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "removable", (device->flags & BLOCK_DEVICE_REMOVABLE) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_bool(&builder, "flush_supported", (device->flags & BLOCK_DEVICE_FLUSH_SUPPORTED) != 0u);
	if (status == SYSCALL_STATUS_OK)
		status = add_string(&builder, "partition_scheme", partition->scheme == PARTITION_SCHEME_GPT ? "gpt" : "mbr");
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_string(&builder, "type_guid", type_guid);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_string(&builder, "unique_guid", unique_guid);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT)
		status = add_u64(&builder, "attributes", partition->gpt.attributes);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_GPT && partition->gpt.name[0] != '\0')
		status = add_string(&builder, "partition_name", partition->gpt.name);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_MBR)
		status = add_u64(&builder, "partition_type", partition->mbr.type);
	if (status == SYSCALL_STATUS_OK && partition->scheme == PARTITION_SCHEME_MBR)
		status = add_bool(&builder, "bootable", partition->mbr.bootable);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(
			&builder, BLOCK_RESOURCE, sizeof(BLOCK_RESOURCE) - 1u, block_cap, block_rights(device));
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static bool wait_for_shutdown(void) {
	channel_id_t endpoint     = CHANNEL_ID_INVALID;
	cap_id_t     activity_cap = CAP_ID_INVALID;

	if (channel_create(&endpoint, &activity_cap) != SYSCALL_STATUS_OK) return false;
	for (;;) {
		struct signal_message activity;
		syscall_status_t      status = signal_wait(activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) break;
	}
	(void)cap_drop(activity_cap);
	(void)channel_destroy(endpoint);
	return false;
}

bool partition_service_run(cap_id_t device_cap, struct block_device* parent, const struct partition_table* partitions) {
	if (parent == NULL || partitions == NULL || partitions->count == 0u) return false;
	for (size_t index = 0u; index < partitions->count; index++) {
		const struct partition_entry* partition = &partitions->entries[index];
		cap_id_t                      slice_cap = CAP_ID_INVALID;
		if (block_device_slice(parent, partition->first_lba, partition->block_count, &slice_cap) != SYSCALL_STATUS_OK)
			return false;
		bool created = create_child(device_cap, slice_cap, parent, partition);
		(void)cap_drop(slice_cap);
		if (!created) return false;

		char precise[5u + PARTITION_GUID_STRING_SIZE];
		if (partition->scheme == PARTITION_SCHEME_GPT) {
			char guid[PARTITION_GUID_STRING_SIZE];
			partition_format_guid(partition->gpt.type_guid, guid);
			(void)snprintf(precise, sizeof(precise), "gpt:%s", guid);
		}
		else {
			(void)snprintf(precise, sizeof(precise), "mbr:%02x", partition->mbr.type);
		}
		RUNTIME_DEBUG_LOG("partition: published partition-%u as SATA block slice (%s, first_lba=%llu, blocks=%llu)\n",
		                  partition->table_index,
		                  precise,
		                  (unsigned long long)partition->first_lba,
		                  (unsigned long long)partition->block_count);
	}
	printf("partition: published %zu partition device%s\n", partitions->count, partitions->count == 1u ? "" : "s");
	block_device_close(parent);
	return wait_for_shutdown();
}
