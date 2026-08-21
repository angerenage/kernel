#include <core/backing_store.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct backing_store_node {
	uintptr_t entries[BACKING_STORE_NODE_ENTRY_COUNT];
};

_Static_assert(PMM_PAGE_SIZE % sizeof(uintptr_t) == 0u, "backing radix entries must fill one PMM page");
_Static_assert(sizeof(struct backing_store_node) == PMM_PAGE_SIZE, "backing radix node must fill one PMM page");
_Static_assert(BACKING_STORE_NODE_ENTRY_COUNT == ((size_t)1u << BACKING_STORE_RADIX_BITS),
               "backing radix fanout must match the configured radix bits");

static inline struct backing_store_node* backing_node_virt(uintptr_t phys) {
	return (struct backing_store_node*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static size_t backing_store_depth(size_t page_count) {
	size_t highest_page;
	size_t depth = 1u;

	if (page_count == 0u) return 0u;
	highest_page = page_count - 1u;
	while (highest_page >= BACKING_STORE_NODE_ENTRY_COUNT) {
		highest_page /= BACKING_STORE_NODE_ENTRY_COUNT;
		depth++;
	}
	return depth;
}

static bool backing_store_indexes(const struct backing_store* store, size_t page_index,
                                  size_t indexes[BACKING_STORE_MAX_DEPTH]) {
	size_t remaining;

	if (!store || page_index >= store->page_count || store->tree_depth == 0u ||
	    store->tree_depth > BACKING_STORE_MAX_DEPTH)
		return false;
	remaining = page_index;
	for (size_t level = store->tree_depth; level > 0u; level--) {
		indexes[level - 1u] = remaining % BACKING_STORE_NODE_ENTRY_COUNT;
		remaining /= BACKING_STORE_NODE_ENTRY_COUNT;
	}
	return remaining == 0u;
}

static bool backing_node_alloc(uintptr_t* out_phys) {
	uintptr_t phys = 0u;

	if (out_phys) *out_phys = 0u;
	if (!out_phys || !pmm_alloc_pages(1u, &phys)) return false;
	if (phys == 0u) {
		(void)pmm_free_pages(phys, 1u);
		return false;
	}
	memset(backing_node_virt(phys), 0, PMM_PAGE_SIZE);
	*out_phys = phys;
	return true;
}

static bool backing_data_alloc(uintptr_t* out_phys) {
	uintptr_t phys = 0u;

	if (out_phys) *out_phys = 0u;
	if (!out_phys || !pmm_alloc_pages(1u, &phys)) return false;
	if (phys == 0u) {
		(void)pmm_free_pages(phys, 1u);
		return false;
	}
	*out_phys = phys;
	return true;
}

static bool backing_node_is_empty(const struct backing_store_node* node) {
	if (!node) return true;
	for (size_t entry = 0u; entry < BACKING_STORE_NODE_ENTRY_COUNT; entry++) {
		if (node->entries[entry] != 0u) return false;
	}
	return true;
}

void backing_store_init(struct backing_store* store, size_t page_count) {
	if (!store) return;
	*store = (struct backing_store){
		.page_count = page_count,
		.tree_depth = backing_store_depth(page_count),
	};
}

bool backing_store_page_phys(const struct backing_store* store, size_t page_index, uintptr_t* out_phys) {
	size_t    indexes[BACKING_STORE_MAX_DEPTH];
	uintptr_t node_phys;

	if (out_phys) *out_phys = 0u;
	if (!out_phys || !backing_store_indexes(store, page_index, indexes) || store->root_phys == 0u) return false;
	node_phys = store->root_phys;
	for (size_t level = 0u; level < store->tree_depth; level++) {
		const struct backing_store_node* node  = backing_node_virt(node_phys);
		uintptr_t                        entry = node->entries[indexes[level]];

		if (entry == 0u) return false;
		if (level + 1u == store->tree_depth) {
			*out_phys = entry;
			return true;
		}
		node_phys = entry;
	}
	return false;
}

static void backing_store_discard_unpublished(const uintptr_t* nodes, size_t node_count) {
	for (size_t node = 0u; node < node_count; node++) {
		if (nodes[node] != 0u) (void)pmm_free_pages(nodes[node], 1u);
	}
}

bool backing_store_ensure_page(struct backing_store* store, size_t page_index, uintptr_t* out_phys,
                               bool* out_allocated) {
	size_t                     indexes[BACKING_STORE_MAX_DEPTH];
	uintptr_t                  new_nodes[BACKING_STORE_MAX_DEPTH] = {0};
	struct backing_store_node* attach_parent                      = NULL;
	size_t                     attach_index                       = 0u;
	size_t                     first_missing_level                = 0u;
	size_t                     new_node_count;
	size_t                     allocated_node_count = 0u;
	uintptr_t                  data_phys            = 0u;

	if (out_phys) *out_phys = 0u;
	if (out_allocated) *out_allocated = false;
	if (!out_phys || !backing_store_indexes(store, page_index, indexes)) return false;

	if (store->root_phys != 0u) {
		uintptr_t node_phys = store->root_phys;

		for (size_t level = 0u; level < store->tree_depth; level++) {
			struct backing_store_node* node  = backing_node_virt(node_phys);
			uintptr_t*                 entry = &node->entries[indexes[level]];

			if (level + 1u == store->tree_depth) {
				if (*entry != 0u) {
					*out_phys = *entry;
					return true;
				}
				if (!backing_data_alloc(&data_phys)) return false;
				*entry = data_phys;
				store->resident_page_count++;
				*out_phys = data_phys;
				if (out_allocated) *out_allocated = true;
				return true;
			}
			if (*entry == 0u) {
				attach_parent       = node;
				attach_index        = indexes[level];
				first_missing_level = level + 1u;
				break;
			}
			node_phys = *entry;
		}
	}

	new_node_count = store->tree_depth - first_missing_level;
	for (size_t node = 0u; node < new_node_count; node++) {
		if (!backing_node_alloc(&new_nodes[node])) {
			backing_store_discard_unpublished(new_nodes, allocated_node_count);
			return false;
		}
		allocated_node_count++;
	}
	if (!backing_data_alloc(&data_phys)) {
		backing_store_discard_unpublished(new_nodes, allocated_node_count);
		return false;
	}
	for (size_t node = 0u; node < new_node_count; node++) {
		size_t                     level      = first_missing_level + node;
		struct backing_store_node* radix_node = backing_node_virt(new_nodes[node]);

		radix_node->entries[indexes[level]] = node + 1u < new_node_count ? new_nodes[node + 1u] : data_phys;
	}
	if (attach_parent != NULL) attach_parent->entries[attach_index] = new_nodes[0];
	else store->root_phys = new_nodes[0];
	store->metadata_page_count += new_node_count;
	store->resident_page_count++;
	*out_phys = data_phys;
	if (out_allocated) *out_allocated = true;
	return true;
}

bool backing_store_release_page(struct backing_store* store, size_t page_index) {
	size_t                     indexes[BACKING_STORE_MAX_DEPTH];
	uintptr_t                  path_phys[BACKING_STORE_MAX_DEPTH];
	struct backing_store_node* path_nodes[BACKING_STORE_MAX_DEPTH];
	uintptr_t                  node_phys;

	if (!backing_store_indexes(store, page_index, indexes)) return false;
	if (store->root_phys == 0u) return true;
	node_phys = store->root_phys;
	for (size_t level = 0u; level < store->tree_depth; level++) {
		struct backing_store_node* node = backing_node_virt(node_phys);
		uintptr_t                  entry;

		path_phys[level]  = node_phys;
		path_nodes[level] = node;
		entry             = node->entries[indexes[level]];
		if (entry == 0u) return true;
		if (level + 1u == store->tree_depth) {
			if (store->resident_page_count == 0u) return false;
			if (!pmm_free_pages(entry, 1u)) return false;
			node->entries[indexes[level]] = 0u;
			store->resident_page_count--;
		}
		else node_phys = entry;
	}

	for (size_t level = store->tree_depth; level > 0u; level--) {
		size_t current = level - 1u;

		if (!backing_node_is_empty(path_nodes[current])) break;
		if (store->metadata_page_count == 0u) return false;
		if (!pmm_free_pages(path_phys[current], 1u)) return false;
		store->metadata_page_count--;
		if (current == 0u) store->root_phys = 0u;
		else path_nodes[current - 1u]->entries[indexes[current - 1u]] = 0u;
	}
	return true;
}

static void backing_store_release_node(uintptr_t node_phys, size_t level, size_t tree_depth) {
	struct backing_store_node* node = backing_node_virt(node_phys);

	for (size_t entry = 0u; entry < BACKING_STORE_NODE_ENTRY_COUNT; entry++) {
		uintptr_t child = node->entries[entry];

		if (child == 0u) continue;
		if (level + 1u == tree_depth) (void)pmm_free_pages(child, 1u);
		else backing_store_release_node(child, level + 1u, tree_depth);
	}
	(void)pmm_free_pages(node_phys, 1u);
}

void backing_store_release(struct backing_store* store) {
	size_t page_count;

	if (!store) return;
	page_count = store->page_count;
	if (store->root_phys != 0u) backing_store_release_node(store->root_phys, 0u, store->tree_depth);
	backing_store_init(store, page_count);
}
