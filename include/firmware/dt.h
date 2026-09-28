#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque structure-block position identifying one Device Tree node. */
struct dt_node {
	size_t id;
};

/* Stable zero-copy view of one Device Tree property value. */
struct dt_property {
	const void* data;
	size_t      size;
};

/* Invalid node returned when traversal cannot produce another node. */
#define DT_NODE_INVALID ((struct dt_node){.id = SIZE_MAX})

/* Validate the supplied HHDM FDT and publish its immutable structure once. */
bool dt_init(const void* dtb);

/* Return whether node identifies a node in the initialized Device Tree. */
bool dt_node_valid(struct dt_node node);

/* Return the Device Tree root node, or DT_NODE_INVALID when unavailable. */
struct dt_node dt_root(void);

/* Return the first direct child of node, or DT_NODE_INVALID when absent. */
struct dt_node dt_node_child(struct dt_node node);

/* Return the next sibling of node, or DT_NODE_INVALID when absent. */
struct dt_node dt_node_next(struct dt_node node);

/* Return node's parent, or DT_NODE_INVALID for the root or an invalid node. */
struct dt_node dt_node_parent(struct dt_node node);

/* Return node's stable name string, or NULL for an invalid node. */
const char* dt_node_name(struct dt_node node);

/* Return a stable view of node's named property without copying it. */
bool dt_node_property(struct dt_node node, const char* name, struct dt_property* out);

/* Decode one or two consecutive big-endian cells from a property. */
bool dt_property_read_cells(const struct dt_property* property, size_t first_cell, size_t cell_count, uint64_t* out);

/* Return whether a complete property string list contains value. */
bool dt_property_string_list_contains(const struct dt_property* property, const char* value);

/* Return whether node is enabled according to its status property. */
bool dt_node_enabled(struct dt_node node);

/* Return whether node's compatible string list contains compatible. */
bool dt_node_compatible(struct dt_node node, const char* compatible);

/* Return the unique node carrying phandle, or DT_NODE_INVALID. */
struct dt_node dt_node_by_phandle(uint32_t phandle);
