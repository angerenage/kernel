#include "volume.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FAT32_MIN_CLUSTERS 65525u
#define FAT32_MAX_CLUSTERS 0x0fffffeeu
#define FAT32_END_OF_CHAIN 0x0ffffff8u
#define FAT32_BAD_CLUSTER 0x0ffffff7u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT32_BOOT_SIGNATURE_OFFSET 510u
#define FAT32_FSINFO_LEAD_SIGNATURE 0x41615252u
#define FAT32_FSINFO_STRUCTURE_SIGNATURE 0x61417272u
#define FAT32_FSINFO_TRAIL_SIGNATURE 0xaa550000u
#define FAT32_IO_BUFFER_BLOCKS 128u

static uint16_t read_u16_le(const uint8_t* value) {
	return (uint16_t)value[0] | (uint16_t)value[1] << 8u;
}

static uint32_t read_u32_le(const uint8_t* value) {
	return (uint32_t)value[0] | (uint32_t)value[1] << 8u | (uint32_t)value[2] << 16u | (uint32_t)value[3] << 24u;
}

static bool power_of_two(uint32_t value) {
	return value != 0u && (value & (value - 1u)) == 0u;
}

static void volume_reset(struct fat32_volume* volume) {
	*volume = (struct fat32_volume){
		.device = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID},
		.buffer = {.memory_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .mapping_cap = CAP_ID_INVALID},
		.fat_buffer = {.memory_cap        = CAP_ID_INVALID,
		           .address_space_cap = CAP_ID_INVALID,
		           .mapping_cap       = CAP_ID_INVALID},
	};
}

void fat32_volume_close(struct fat32_volume* volume) {
	if (volume == NULL) return;
	block_buffer_release(&volume->device, &volume->fat_buffer);
	block_buffer_release(&volume->device, &volume->buffer);
	block_device_close(&volume->device);
	volume_reset(volume);
}

enum fat32_result fat32_volume_read_sector(struct fat32_volume* volume, uint64_t sector) {
	if (volume == NULL || sector >= volume->total_sectors) return FAT32_RESULT_UNSUPPORTED;
	return block_read(&volume->device, &volume->buffer, 0u, sector, 1u) == SYSCALL_STATUS_OK ? FAT32_RESULT_OK
	                                                                                         : FAT32_RESULT_ERROR;
}

enum fat32_result fat32_volume_write_sector(struct fat32_volume* volume, uint64_t sector) {
	if (volume == NULL || sector >= volume->total_sectors || (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	return block_write(&volume->device, &volume->buffer, 0u, sector, 1u) == SYSCALL_STATUS_OK ? FAT32_RESULT_OK
	                                                                                          : FAT32_RESULT_ERROR;
}

bool fat32_volume_cluster_valid(const struct fat32_volume* volume, uint32_t cluster) {
	return volume != NULL && cluster >= 2u && cluster - 2u < volume->cluster_count;
}

uint64_t fat32_volume_cluster_sector(const struct fat32_volume* volume, uint32_t cluster) {
	return volume->data_start + (uint64_t)(cluster - 2u) * volume->sectors_per_cluster;
}

enum fat32_result fat32_volume_read_fat(struct fat32_volume* volume, uint32_t cluster, uint32_t* out_value) {
	if (!fat32_volume_cluster_valid(volume, cluster) || out_value == NULL) return FAT32_RESULT_UNSUPPORTED;
	uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
	uint64_t sector      = volume->fat_start + byte_offset / volume->device.block_size;
	uint32_t offset      = (uint32_t)(byte_offset % volume->device.block_size);

	if (offset > volume->device.block_size - sizeof(uint32_t)) return FAT32_RESULT_UNSUPPORTED;
	if (!volume->fat_cache_valid || volume->cached_fat_sector != sector) {
		if (block_read(&volume->device, &volume->fat_buffer, 0u, sector, 1u) != SYSCALL_STATUS_OK)
			return FAT32_RESULT_ERROR;
		volume->cached_fat_sector = sector;
		volume->fat_cache_valid   = true;
	}
	*out_value = read_u32_le(volume->fat_buffer.address + offset) & FAT32_ENTRY_MASK;
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_volume_write_fat(struct fat32_volume* volume, uint32_t cluster, uint32_t value) {
	if (!fat32_volume_cluster_valid(volume, cluster) || (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u)
		return FAT32_RESULT_UNSUPPORTED;
	uint64_t byte_offset = (uint64_t)cluster * sizeof(uint32_t);
	uint32_t offset      = (uint32_t)(byte_offset % volume->device.block_size);
	uint32_t first_copy =
		volume->fat_mirrored ? 0u : (uint32_t)((volume->fat_start - volume->fat_base) / volume->fat_size);
	uint32_t copy_count = volume->fat_mirrored ? volume->fat_count : first_copy + 1u;

	if (offset > volume->device.block_size - sizeof(uint32_t)) return FAT32_RESULT_UNSUPPORTED;
	for (uint32_t copy = first_copy; copy < copy_count; copy++) {
		uint64_t sector =
			volume->fat_base + (uint64_t)copy * volume->fat_size + byte_offset / volume->device.block_size;
		if (block_read(&volume->device, &volume->fat_buffer, 0u, sector, 1u) != SYSCALL_STATUS_OK)
			return FAT32_RESULT_ERROR;
		uint32_t original = read_u32_le(volume->fat_buffer.address + offset);
		uint32_t encoded  = (original & ~FAT32_ENTRY_MASK) | (value & FAT32_ENTRY_MASK);
		for (size_t index = 0u; index < sizeof(encoded); index++)
			volume->fat_buffer.address[offset + index] = (uint8_t)(encoded >> (index * 8u));
		if (block_write(&volume->device, &volume->fat_buffer, 0u, sector, 1u) != SYSCALL_STATUS_OK)
			return FAT32_RESULT_ERROR;
		volume->fat_cache_valid = false;
	}
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_volume_next_cluster(struct fat32_volume* volume, uint32_t cluster, uint32_t* out_next,
                                            bool* out_end) {
	if (out_next == NULL || out_end == NULL) return FAT32_RESULT_UNSUPPORTED;
	uint32_t          next;
	enum fat32_result result = fat32_volume_read_fat(volume, cluster, &next);
	if (result != FAT32_RESULT_OK) return result;
	if (next >= FAT32_END_OF_CHAIN) {
		*out_next = 0u;
		*out_end  = true;
		return FAT32_RESULT_OK;
	}
	if (next == FAT32_BAD_CLUSTER || !fat32_volume_cluster_valid(volume, next)) return FAT32_RESULT_UNSUPPORTED;
	*out_next = next;
	*out_end  = false;
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_volume_zero_cluster(struct fat32_volume* volume, uint32_t cluster) {
	if (!fat32_volume_cluster_valid(volume, cluster)) return FAT32_RESULT_UNSUPPORTED;
	uint64_t first = fat32_volume_cluster_sector(volume, cluster);
	memset(volume->buffer.address, 0, volume->device.block_size);
	for (uint32_t sector = 0u; sector < volume->sectors_per_cluster; sector++) {
		enum fat32_result result = fat32_volume_write_sector(volume, first + sector);
		if (result != FAT32_RESULT_OK) return result;
	}
	return FAT32_RESULT_OK;
}

static enum fat32_result invalidate_fsinfo_copy(struct fat32_volume* volume, uint64_t sector) {
	enum fat32_result result = fat32_volume_read_sector(volume, sector);
	if (result != FAT32_RESULT_OK) return result;
	if (read_u32_le(volume->buffer.address) != FAT32_FSINFO_LEAD_SIGNATURE ||
	    read_u32_le(volume->buffer.address + 484u) != FAT32_FSINFO_STRUCTURE_SIGNATURE ||
	    read_u32_le(volume->buffer.address + 508u) != FAT32_FSINFO_TRAIL_SIGNATURE)
		return FAT32_RESULT_UNSUPPORTED;
	memset(volume->buffer.address + 488u, 0xff, 8u);
	return fat32_volume_write_sector(volume, sector);
}

enum fat32_result fat32_volume_invalidate_fsinfo(struct fat32_volume* volume) {
	if (volume == NULL) return FAT32_RESULT_UNSUPPORTED;
	if (volume->fsinfo_sector == UINT16_MAX) return FAT32_RESULT_OK;
	enum fat32_result result = invalidate_fsinfo_copy(volume, volume->fsinfo_sector);
	if (result != FAT32_RESULT_OK) return result;
	if (volume->backup_sector != 0u && volume->backup_sector != UINT16_MAX &&
	    (uint32_t)volume->backup_sector + volume->fsinfo_sector < volume->fat_base) {
		enum fat32_result backup =
			invalidate_fsinfo_copy(volume, (uint32_t)volume->backup_sector + volume->fsinfo_sector);
		if (backup == FAT32_RESULT_ERROR) result = backup;
	}
	return result;
}

enum fat32_result fat32_volume_flush(struct fat32_volume* volume) {
	if (volume == NULL || (volume->device.flags & BLOCK_DEVICE_WRITABLE) == 0u) return FAT32_RESULT_UNSUPPORTED;
	if ((volume->device.flags & BLOCK_DEVICE_FLUSH_SUPPORTED) == 0u) return FAT32_RESULT_OK;
	return block_flush(&volume->device) == SYSCALL_STATUS_OK ? FAT32_RESULT_OK : FAT32_RESULT_ERROR;
}

static enum fat32_result validate_fsinfo(struct fat32_volume* volume, uint16_t sector, uint16_t reserved_sectors,
                                         uint32_t* out_hint) {
	*out_hint = 2u;
	if (sector == UINT16_MAX) return FAT32_RESULT_OK;
	if (sector == 0u || sector >= reserved_sectors || sector >= volume->total_sectors) return FAT32_RESULT_UNSUPPORTED;
	enum fat32_result result = fat32_volume_read_sector(volume, sector);
	if (result != FAT32_RESULT_OK) return result;
	if (read_u32_le(volume->buffer.address) != FAT32_FSINFO_LEAD_SIGNATURE ||
	    read_u32_le(volume->buffer.address + 484u) != FAT32_FSINFO_STRUCTURE_SIGNATURE ||
	    read_u32_le(volume->buffer.address + 508u) != FAT32_FSINFO_TRAIL_SIGNATURE)
		return FAT32_RESULT_UNSUPPORTED;
	uint32_t hint = read_u32_le(volume->buffer.address + 492u);
	if (fat32_volume_cluster_valid(volume, hint)) *out_hint = hint;
	return FAT32_RESULT_OK;
}

enum fat32_result fat32_volume_open(cap_id_t device_cap, struct fat32_volume* out_volume) {
	if (out_volume == NULL) return FAT32_RESULT_ERROR;
	volume_reset(out_volume);
	if (block_device_open(device_cap, &out_volume->device) != SYSCALL_STATUS_OK &&
	    block_device_open_read_only(device_cap, &out_volume->device) != SYSCALL_STATUS_OK)
		return FAT32_RESULT_ERROR;
	uint32_t block_size    = out_volume->device.block_size;
	uint32_t buffer_blocks = out_volume->device.max_transfer_blocks < FAT32_IO_BUFFER_BLOCKS
	                             ? out_volume->device.max_transfer_blocks
	                             : FAT32_IO_BUFFER_BLOCKS;
	if (block_size < 512u || block_size > 4096u || !power_of_two(block_size) ||
	    block_buffer_allocate(&out_volume->device, block_size * buffer_blocks, &out_volume->buffer) !=
	        SYSCALL_STATUS_OK ||
	    block_buffer_allocate(&out_volume->device, block_size, &out_volume->fat_buffer) != SYSCALL_STATUS_OK) {
		fat32_volume_close(out_volume);
		return block_size < 512u || block_size > 4096u || !power_of_two(block_size) ? FAT32_RESULT_UNSUPPORTED
		                                                                            : FAT32_RESULT_ERROR;
	}
	out_volume->total_sectors = out_volume->device.block_count;
	if (fat32_volume_read_sector(out_volume, 0u) != FAT32_RESULT_OK) {
		fat32_volume_close(out_volume);
		return FAT32_RESULT_ERROR;
	}
	const uint8_t* boot                = out_volume->buffer.address;
	uint16_t       bytes_per_sector    = read_u16_le(boot + 11u);
	uint32_t       sectors_per_cluster = boot[13u];
	uint16_t       reserved_sectors    = read_u16_le(boot + 14u);
	uint32_t       fat_count           = boot[16u];
	uint16_t       root_entries        = read_u16_le(boot + 17u);
	uint16_t       total_sectors_16    = read_u16_le(boot + 19u);
	uint16_t       fat_size_16         = read_u16_le(boot + 22u);
	uint32_t       total_sectors       = read_u32_le(boot + 32u);
	uint32_t       fat_size            = read_u32_le(boot + 36u);
	uint16_t       flags               = read_u16_le(boot + 40u);
	uint16_t       version             = read_u16_le(boot + 42u);
	uint32_t       root_cluster        = read_u32_le(boot + 44u);
	uint16_t       fsinfo_sector       = read_u16_le(boot + 48u);
	uint16_t       backup_sector       = read_u16_le(boot + 50u);
	uint32_t       active_fat          = (flags & 0x0080u) != 0u ? flags & 0x000fu : 0u;
	uint64_t       fat_sectors         = (uint64_t)fat_count * fat_size;
	uint64_t       data_start          = (uint64_t)reserved_sectors + fat_sectors;
	uint64_t       data_sectors        = total_sectors > data_start ? total_sectors - data_start : 0u;
	uint64_t       cluster_count       = sectors_per_cluster == 0u ? 0u : data_sectors / sectors_per_cluster;
	uint64_t       fat_entries         = (uint64_t)fat_size * bytes_per_sector / sizeof(uint32_t);

	if ((boot[0u] != 0xebu && boot[0u] != 0xe9u) || bytes_per_sector != block_size ||
	    !power_of_two(sectors_per_cluster) || sectors_per_cluster > 128u || reserved_sectors == 0u || fat_count == 0u ||
	    fat_count > 8u || root_entries != 0u || total_sectors_16 != 0u || fat_size_16 != 0u || total_sectors == 0u ||
	    total_sectors > out_volume->device.block_count || fat_size == 0u || (flags & 0xff70u) != 0u ||
	    active_fat >= fat_count || version != 0u ||
	    (backup_sector != 0u && backup_sector != UINT16_MAX && backup_sector >= reserved_sectors) ||
	    boot[FAT32_BOOT_SIGNATURE_OFFSET] != 0x55u || boot[FAT32_BOOT_SIGNATURE_OFFSET + 1u] != 0xaau ||
	    (boot[66u] != 0x28u && boot[66u] != 0x29u) || memcmp(boot + 82u, "FAT32   ", 8u) != 0 ||
	    data_start >= total_sectors || cluster_count < FAT32_MIN_CLUSTERS || cluster_count > FAT32_MAX_CLUSTERS ||
	    fat_entries < cluster_count + 2u || root_cluster < 2u || root_cluster - 2u >= cluster_count) {
		printf("fat32: partition does not contain a valid FAT32 volume\n");
		fat32_volume_close(out_volume);
		return FAT32_RESULT_UNSUPPORTED;
	}
	out_volume->total_sectors       = total_sectors;
	out_volume->fat_base            = reserved_sectors;
	out_volume->fat_start           = (uint64_t)reserved_sectors + (uint64_t)active_fat * fat_size;
	out_volume->data_start          = data_start;
	out_volume->fat_size            = fat_size;
	out_volume->fat_count           = fat_count;
	out_volume->sectors_per_cluster = sectors_per_cluster;
	out_volume->cluster_count       = (uint32_t)cluster_count;
	out_volume->root_cluster        = root_cluster;
	out_volume->volume_id           = read_u32_le(boot + 67u);
	out_volume->fsinfo_sector       = fsinfo_sector;
	out_volume->backup_sector       = backup_sector;
	out_volume->fat_mirrored        = (flags & 0x0080u) == 0u;
	memcpy(out_volume->label, boot + 71u, 11u);
	out_volume->label[11u] = '\0';
	for (size_t index = 0u; index < 11u; index++)
		if ((uint8_t)out_volume->label[index] < 0x20u || (uint8_t)out_volume->label[index] > 0x7eu)
			out_volume->label[index] = '?';
	for (size_t size = 11u; size != 0u && out_volume->label[size - 1u] == ' '; size--)
		out_volume->label[size - 1u] = '\0';
	enum fat32_result result =
		validate_fsinfo(out_volume, fsinfo_sector, reserved_sectors, &out_volume->allocation_hint);
	if (result != FAT32_RESULT_OK) {
		printf("fat32: FAT32 FSInfo metadata is invalid\n");
		fat32_volume_close(out_volume);
		return result;
	}
	printf("fat32: volume label=\"%s\" sectors=%u sector_size=%u cluster_size=%u root_cluster=%u\n",
	       out_volume->label,
	       total_sectors,
	       block_size,
	       sectors_per_cluster * block_size,
	       root_cluster);
	return FAT32_RESULT_OK;
}
