#pragma once

#include <base/cap.h>
#include <base/device_tree.h>
#include <base/syscall.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Indexed raw property metadata used by structural translators. */
struct dt_parser_property {
	uint64_t index;
	uint64_t value_size;
};

/* One translated address and length from a reg tuple. */
struct dt_parser_range {
	uint64_t base;
	uint64_t length;
};

/* Owned collection of translated reg tuples. */
struct dt_parser_ranges {
	struct dt_parser_range* values;
	size_t                  count;
};

/* One canonical fixed interrupt translated by the Device Tree provider. */
struct dt_parser_interrupt {
	uint64_t                controller_register_address;
	uint32_t                local_source_id;
	enum interrupt_trigger  trigger;
	enum interrupt_polarity polarity;
};

/* Owned collection of firmware-described fixed interrupts. */
struct dt_parser_interrupts {
	struct dt_parser_interrupt* values;
	size_t                      count;
};

/* One canonical IOMMU input source translated by the Device Tree provider. */
struct dt_parser_dma_source {
	uint64_t controller_register_address;
	uint32_t local_source_id;
};

/* Owned collection of firmware-described IOMMU input sources. */
struct dt_parser_dma_sources {
	struct dt_parser_dma_source* values;
	size_t                       count;
};

/* Owned length-prefixed compatible-ID sequence in provider order. */
struct dt_parser_string_list {
	uint8_t* value;
	size_t   value_size;
	size_t   count;
};

/* Decode one canonical little-endian 32-bit value. */
uint32_t dt_parser_read_u32_le(const uint8_t value[4]);

/* Encode one canonical little-endian 32-bit value. */
void dt_parser_write_u32_le(uint8_t value[4], uint32_t input);

/* Validate one length-delimited, NUL-free UTF-8 string. */
bool dt_parser_utf8_valid(const uint8_t* value, size_t size);

/* Return the next visible node in depth-first traversal order. */
syscall_status_t dt_parser_walk_next(cap_id_t provider_cap, device_tree_node_id_t node,
                                     device_tree_node_id_t* out_next);

/* Report whether a node and every visible ancestor have an enabled status. */
syscall_status_t dt_parser_node_enabled(cap_id_t provider_cap, device_tree_node_id_t node, bool* out_enabled);

/* Find one property by its exact Device Tree name. */
syscall_status_t dt_parser_property_find(cap_id_t provider_cap, device_tree_node_id_t node, const char* name,
                                         struct dt_parser_property* out_property);

/* Read one exact range from indexed property metadata. */
syscall_status_t dt_parser_property_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                         const struct dt_parser_property* property, uint64_t offset, void* buffer,
                                         size_t size);

/* Decode and translate every structurally valid reg tuple for a node. */
syscall_status_t dt_parser_ranges_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                       struct dt_parser_ranges* out_ranges);

/* Release a translated reg collection. */
void dt_parser_ranges_deinit(struct dt_parser_ranges* ranges);

/* Read every interrupt that the firmware provider can translate for a node. */
syscall_status_t dt_parser_interrupts_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                           struct dt_parser_interrupts* out_interrupts);

/* Release a translated interrupt collection. */
void dt_parser_interrupts_deinit(struct dt_parser_interrupts* interrupts);

/* Read every IOMMU input source that the firmware provider can translate for a node. */
syscall_status_t dt_parser_dma_sources_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                            struct dt_parser_dma_sources* out_sources);

/* Release a translated DMA-source collection. */
void dt_parser_dma_sources_deinit(struct dt_parser_dma_sources* sources);

/* Decode a node's ordered, unique, UTF-8 compatible IDs. */
syscall_status_t dt_parser_compatibles_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                            struct dt_parser_string_list* out_compatibles);

/* Release an encoded compatible-ID collection. */
void dt_parser_string_list_deinit(struct dt_parser_string_list* list);
