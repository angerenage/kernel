#pragma once

#include <stdbool.h>
#include <stdint.h>

struct sata_transport;

/* Addressing and command capabilities reported by IDENTIFY DEVICE. */
struct sata_device_info {
	uint64_t sector_count;
	uint32_t logical_sector_size;
	bool     dma;
	bool     lba48;
	bool     flush_cache;
	bool     flush_cache_ext;
};

/* Issue IDENTIFY DEVICE and report the disk identity and basic capabilities. */
bool sata_identify_device(const struct sata_transport* transport, struct sata_device_info* out_info);
