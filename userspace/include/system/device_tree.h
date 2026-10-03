#pragma once

#include <base/cap.h>
#include <base/device_tree.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Return the root of the userspace-visible Device Tree. */
syscall_status_t device_tree_root(cap_id_t provider_cap, device_tree_node_id_t* out_root);

/* Return navigation and metadata for one visible node. */
syscall_status_t device_tree_node_info(cap_id_t provider_cap, device_tree_node_id_t node,
                                       struct device_tree_node_info_response* out_info);

/* Return the name and value sizes of one indexed property. */
syscall_status_t device_tree_property_info(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           struct device_tree_property_info_response* out_info);

/* Read an exact range from a node name. */
syscall_status_t device_tree_node_name_read(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t offset,
                                            void* buffer, size_t size);

/* Read an exact range from an indexed property name. */
syscall_status_t device_tree_property_name_read(cap_id_t provider_cap, device_tree_node_id_t node,
                                                uint64_t property_index, uint64_t offset, void* buffer, size_t size);

/* Read an exact range from an indexed raw property value. */
syscall_status_t device_tree_property_read(cap_id_t provider_cap, device_tree_node_id_t node, uint64_t property_index,
                                           uint64_t offset, void* buffer, size_t size);

/* Resolve a firmware phandle without exposing hidden nodes. */
syscall_status_t device_tree_resolve_phandle(cap_id_t provider_cap, uint32_t phandle,
                                             struct device_tree_resolve_phandle_response* out_reference);
