#pragma once

#include <stdbool.h>
#include <stdint.h>

struct sata_buffer;
struct sata_transport;

/* Read sectors with READ DMA EXT. */
bool sata_read_dma_ext(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                       uint64_t lba, uint16_t sector_count, uint32_t sector_size);

/* Write sectors with WRITE DMA EXT. */
bool sata_write_dma_ext(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                        uint64_t lba, uint16_t sector_count, uint32_t sector_size);

/* Commit cached writes with FLUSH CACHE EXT. */
bool sata_flush_cache_ext(const struct sata_transport* transport);

/* Read up to 256 sectors with READ DMA using 28-bit LBA addressing. */
bool sata_read_dma(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                   uint32_t lba, uint16_t sector_count, uint32_t sector_size);

/* Write up to 256 sectors with WRITE DMA using 28-bit LBA addressing. */
bool sata_write_dma(const struct sata_transport* transport, struct sata_buffer* buffer, uint32_t buffer_offset,
                    uint32_t lba, uint16_t sector_count, uint32_t sector_size);

/* Commit cached writes with FLUSH CACHE. */
bool sata_flush_cache(const struct sata_transport* transport);
