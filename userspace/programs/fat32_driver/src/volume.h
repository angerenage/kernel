#pragma once

#include <base/cap.h>
#include <runtime/block.h>
#include <stdbool.h>
#include <stdint.h>

enum fat32_result {
	FAT32_RESULT_OK = 0u,
	FAT32_RESULT_NOT_FOUND,
	FAT32_RESULT_ALREADY_EXISTS,
	FAT32_RESULT_NOT_DIRECTORY,
	FAT32_RESULT_NOT_EMPTY,
	FAT32_RESULT_UNSUPPORTED,
	FAT32_RESULT_ERROR,
};

/* Validated geometry with reusable bulk-I/O and FAT-sector buffers. */
struct fat32_volume {
	struct block_device device;
	struct block_buffer buffer;
	struct block_buffer fat_buffer;
	uint64_t            total_sectors;
	uint64_t            fat_base;
	uint64_t            fat_start;
	uint64_t            data_start;
	uint32_t            fat_size;
	uint32_t            fat_count;
	uint32_t            sectors_per_cluster;
	uint32_t            cluster_count;
	uint32_t            root_cluster;
	uint32_t            volume_id;
	char                label[12];
	uint32_t            allocation_hint;
	uint64_t            cached_fat_sector;
	uint16_t            fsinfo_sector;
	uint16_t            backup_sector;
	bool                fat_mirrored;
	bool                fat_cache_valid;
};

/* Open a block partition and validate its FAT32 boot metadata. */
enum fat32_result fat32_volume_open(cap_id_t device_cap, struct fat32_volume* out_volume);

/* Release the reusable buffers and block session. */
void fat32_volume_close(struct fat32_volume* volume);

/* Read one volume-relative logical sector into the reusable buffer. */
enum fat32_result fat32_volume_read_sector(struct fat32_volume* volume, uint64_t sector);

/* Write the reusable buffer to one volume-relative logical sector. */
enum fat32_result fat32_volume_write_sector(struct fat32_volume* volume, uint64_t sector);

/* Read one masked FAT32 entry from the selected FAT. */
enum fat32_result fat32_volume_read_fat(struct fat32_volume* volume, uint32_t cluster, uint32_t* out_value);

/* Update one entry in every active FAT copy. */
enum fat32_result fat32_volume_write_fat(struct fat32_volume* volume, uint32_t cluster, uint32_t value);

/* Resolve one FAT entry and report whether it terminates the chain. */
enum fat32_result fat32_volume_next_cluster(struct fat32_volume* volume, uint32_t cluster, uint32_t* out_next,
                                            bool* out_end);

/* Fill every sector in one data cluster with zeroes. */
enum fat32_result fat32_volume_zero_cluster(struct fat32_volume* volume, uint32_t cluster);

/* Mark the primary and any valid backup FSInfo allocation hints unknown. */
enum fat32_result fat32_volume_invalidate_fsinfo(struct fat32_volume* volume);

/* Commit completed writes when the provider supports flushing. */
enum fat32_result fat32_volume_flush(struct fat32_volume* volume);

/* Return whether a cluster number addresses the validated data region. */
bool fat32_volume_cluster_valid(const struct fat32_volume* volume, uint32_t cluster);

/* Translate a validated data cluster to its first logical sector. */
uint64_t fat32_volume_cluster_sector(const struct fat32_volume* volume, uint32_t cluster);
