#pragma once

#include <hal/device_tree.h>

/* List Device Tree nodes consumed by the RISC-V IOMMU implementation. */
size_t riscv64_iommu_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);
