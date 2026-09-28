#pragma once

#include <firmware/dt.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoded register range belonging to one Device Tree node. */
struct dt_reg {
	uint64_t address;
	uint64_t size;
};

/* Return the number of enabled nodes carrying compatible. */
size_t dt_device_count(const char* compatible);

/* Return an enabled compatible node by stable tree order. */
struct dt_node dt_device_at(const char* compatible, size_t index);

/* Return a node whose named string list contains value, in stable tree order. */
struct dt_node dt_node_with_string_at(const char* name, const char* value, size_t index);

/* Decode one reg entry without translating it through parent buses. */
bool dt_node_reg_raw(struct dt_node node, size_t index, struct dt_reg* out);

/* Decode one reg entry and translate its address into the root address space. */
bool dt_node_reg(struct dt_node node, size_t index, struct dt_reg* out);
