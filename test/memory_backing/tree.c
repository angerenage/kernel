#include <criterion/criterion.h>
#include <string.h>

#include "../../core/memory/memory_backing_tree.h"

Test(memory_backing_tree, ordered_randomized_lookup_split_and_remove) {
	enum { NODE_COUNT = 257 };
	struct memory_backing_rb_tree tree = {0};
	struct memory_backing_extent  nodes[NODE_COUNT];
	struct memory_backing_extent  split = {0};
	memset(nodes, 0, sizeof(nodes));
	for (size_t step = 0u; step < NODE_COUNT; step++) {
		size_t index                = step * 73u % NODE_COUNT;
		nodes[index].logical_start  = index * 8192u;
		nodes[index].size           = index == 0u ? 8192u : 4096u;
		nodes[index].physical_start = index * 12288u;
		cr_assert(memory_backing_tree_insert(&tree, &nodes[index]));
		cr_assert(memory_backing_tree_valid(&tree));
	}
	for (size_t index = 0u; index < NODE_COUNT; index++) {
		cr_assert_eq(memory_backing_tree_find(&tree, index * 8192u), &nodes[index]);
		cr_assert_eq(memory_backing_tree_find(&tree, index * 8192u + 4095u), &nodes[index]);
		if (index != 0u) cr_assert_null(memory_backing_tree_find(&tree, index * 8192u + 4096u));
		if (index != 0u) cr_assert_eq(memory_backing_tree_previous(&nodes[index]), &nodes[index - 1u]);
		if (index + 1u != NODE_COUNT) cr_assert_eq(memory_backing_tree_next(&nodes[index]), &nodes[index + 1u]);
	}
	cr_assert_not(memory_backing_tree_insert(
		&tree, &(struct memory_backing_extent){.logical_start = 8192u, .size = 4096u, .physical_start = 0x1000u}));
	cr_assert(memory_backing_tree_split_at(&tree, 4096u, &split));
	cr_assert_eq(nodes[0].size, 4096u);
	cr_assert_eq(split.logical_start, 4096u);
	cr_assert_eq(split.physical_start, 4096u);
	cr_assert(memory_backing_tree_valid(&tree));
	memory_backing_tree_remove(&tree, &split);
	for (size_t step = 0u; step < NODE_COUNT; step++) {
		size_t index = step * 149u % NODE_COUNT;
		memory_backing_tree_remove(&tree, &nodes[index]);
		cr_assert(memory_backing_tree_valid(&tree));
	}
	cr_assert_null(tree.root);
}

Test(memory_backing_tree, ascending_and_descending_insertions_stay_balanced) {
	struct memory_backing_rb_tree ascending = {0}, descending = {0};
	struct memory_backing_extent  ascending_nodes[128] = {0}, descending_nodes[128] = {0};
	for (size_t i = 0u; i < 128u; i++) {
		ascending_nodes[i] = (struct memory_backing_extent){.logical_start = i * 8192u, .size = 4096u};
		cr_assert(memory_backing_tree_insert(&ascending, &ascending_nodes[i]));
		cr_assert(memory_backing_tree_valid(&ascending));
		descending_nodes[127u - i] = (struct memory_backing_extent){.logical_start = (127u - i) * 8192u, .size = 4096u};
		cr_assert(memory_backing_tree_insert(&descending, &descending_nodes[127u - i]));
		cr_assert(memory_backing_tree_valid(&descending));
	}
}
