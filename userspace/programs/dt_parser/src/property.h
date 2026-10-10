#pragma once

#include <base/cap.h>
#include <base/device_tree.h>
#include <base/syscall.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One normalized property name already reserved within a device builder. */
struct dt_parser_name {
	struct dt_parser_name* next;
	size_t                 size;
	char                   value[];
};

/* Names reserved by automatically translated properties. */
struct dt_parser_name_set {
	struct dt_parser_name* first;
};

/* Convert one Device Tree name into the device protocol's lower-snake-case form. */
bool dt_parser_name_normalize(const char* source, size_t source_size, char output[DEVICE_IDENTIFIER_MAX],
                              size_t* out_size);

/* Reserve one lower-snake-case name, rejecting canonicalization collisions. */
syscall_status_t dt_parser_name_set_add(struct dt_parser_name_set* names, const char* name, size_t name_size);

/* Release every name retained by a translation transaction. */
void dt_parser_name_set_deinit(struct dt_parser_name_set* names);

/* Normalize and append one non-structural Device Tree property. */
syscall_status_t dt_parser_property_append(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           bool registers_translated, bool interrupts_translated,
                                           const struct device_builder* builder, struct dt_parser_name_set* names);
