#pragma once

#include <base/interrupt.h>
#include <stdint.h>

/* Opaque identifier for one node in the userspace-visible Device Tree. */
typedef uint64_t device_tree_node_id_t;

#define DEVICE_TREE_NODE_INVALID UINT64_MAX

enum device_tree_op {
	DEVICE_TREE_OP_ROOT = 0,
	DEVICE_TREE_OP_NODE_INFO,
	DEVICE_TREE_OP_PROPERTY_INFO,
	DEVICE_TREE_OP_READ,
	DEVICE_TREE_OP_RESOLVE_PHANDLE,
};

struct device_tree_request_header {
	enum device_tree_op op;
};

struct device_tree_root_request {
	struct device_tree_request_header header;
};

struct device_tree_root_response {
	device_tree_node_id_t root;
};

struct device_tree_node_info_request {
	struct device_tree_request_header header;
	uint32_t                          reserved;
	device_tree_node_id_t             node;
};

struct device_tree_node_info_response {
	device_tree_node_id_t parent;
	device_tree_node_id_t first_child;
	device_tree_node_id_t next_sibling;
	uint64_t              name_size;
	uint64_t              property_count;
};

struct device_tree_property_info_request {
	struct device_tree_request_header header;
	uint32_t                          reserved;
	device_tree_node_id_t             node;
	uint64_t                          property_index;
};

struct device_tree_property_info_response {
	uint64_t name_size;
	uint64_t value_size;
};

enum device_tree_read_kind {
	DEVICE_TREE_READ_NODE_NAME = 0,
	DEVICE_TREE_READ_PROPERTY_NAME,
	DEVICE_TREE_READ_PROPERTY_VALUE,
};

struct device_tree_read_request {
	struct device_tree_request_header header;
	enum device_tree_read_kind        kind;
	device_tree_node_id_t             node;
	uint64_t                          property_index;
	uint64_t                          offset;
	uint64_t                          size;
};

enum device_tree_reference_kind {
	DEVICE_TREE_REFERENCE_NODE = 0,
	DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER,
	DEVICE_TREE_REFERENCE_DMA_CONTROLLER,
	DEVICE_TREE_REFERENCE_UNSUPPORTED,
};

#define DEVICE_TREE_REFERENCE_MAX_SPECIFIER_CELLS 4u

/* Resolve a phandle alone or together with its controller-specific specifier cells. */
struct device_tree_resolve_phandle_request {
	struct device_tree_request_header header;
	uint32_t                          phandle;
	uint32_t                          specifier_cell_count;
	uint32_t                          reserved;
	uint32_t                          specifier_cells[DEVICE_TREE_REFERENCE_MAX_SPECIFIER_CELLS];
};

/* Resolved target plus canonical fixed-interrupt claim inputs when interrupt_claim_valid is set. */
struct device_tree_resolve_phandle_response {
	enum device_tree_reference_kind kind;
	uint32_t                        specifier_cell_count;
	uint64_t                        value;
	uint32_t                        interrupt_claim_valid;
	uint32_t                        local_source_id;
	enum interrupt_trigger          trigger;
	enum interrupt_polarity         polarity;
};
