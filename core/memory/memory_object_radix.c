#include "memory_object_radix.h"

#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <string.h>

struct radix_node {
	uintptr_t entries[MEMORY_OBJECT_RADIX_ENTRIES];
};

_Static_assert(sizeof(struct radix_node) == VMM_PAGE_SIZE, "memory-object radix node must fill one page");

static struct radix_node* node_virt(uintptr_t phys) {
	return (struct radix_node*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

uint8_t memory_object_radix_depth(size_t page_count) {
	size_t  highest;
	uint8_t depth = 1u;
	if (page_count == 0u) return 0u;
	highest = page_count - 1u;
	while (highest >= MEMORY_OBJECT_RADIX_ENTRIES) {
		highest /= MEMORY_OBJECT_RADIX_ENTRIES;
		depth++;
	}
	return depth;
}

static void indexes_for(const struct memory_object* object, size_t page_index,
                        size_t indexes[MEMORY_OBJECT_RADIX_MAX_DEPTH]) {
	for (uint8_t level = object->radix_depth; level > 0u; level--) {
		indexes[level - 1u] = page_index & (MEMORY_OBJECT_RADIX_ENTRIES - 1u);
		page_index >>= MEMORY_OBJECT_RADIX_BITS;
	}
}

bool memory_object_radix_lookup(const struct memory_object* object, size_t page_index, uintptr_t* out_phys) {
	size_t    indexes[MEMORY_OBJECT_RADIX_MAX_DEPTH];
	uintptr_t node_phys;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || page_index >= object->page_count || object->radix_depth == 0u ||
	    object->radix_depth > MEMORY_OBJECT_RADIX_MAX_DEPTH || object->backing_root_or_phys == 0u)
		return false;
	indexes_for(object, page_index, indexes);
	node_phys = object->backing_root_or_phys;
	for (uint8_t level = 0u; level < object->radix_depth; level++) {
		uintptr_t entry = node_virt(node_phys)->entries[indexes[level]];
		if (entry == 0u) return false;
		if (level + 1u == object->radix_depth) {
			*out_phys = entry;
			return true;
		}
		node_phys = entry;
	}
	return false;
}

static bool alloc_zeroed_page(uintptr_t* out_phys) {
	struct pmm_extent allocation;
	if (out_phys != NULL) *out_phys = 0u;
	if (out_phys == NULL ||
	    !pmm_alloc(&(const struct pmm_alloc_request){.size = VMM_PAGE_SIZE, .alignment = VMM_PAGE_SIZE}, &allocation))
		return false;
	if (allocation.address == 0u) {
		(void)pmm_free(allocation);
		return false;
	}
	memset(node_virt(allocation.address), 0, VMM_PAGE_SIZE);
	*out_phys = allocation.address;
	return true;
}

static void discard_nodes(const uintptr_t* nodes, size_t count) {
	for (size_t i = 0u; i < count; i++) (void)pmm_free((struct pmm_extent){.address = nodes[i], .size = VMM_PAGE_SIZE});
}

bool memory_object_radix_insert(struct memory_object* object, size_t page_index, uintptr_t phys) {
	size_t             indexes[MEMORY_OBJECT_RADIX_MAX_DEPTH];
	uintptr_t          new_nodes[MEMORY_OBJECT_RADIX_MAX_DEPTH];
	struct radix_node* attach_parent = NULL;
	size_t             attach_index  = 0u;
	uint8_t            missing_level = 0u;
	size_t             new_count;

	if (object == NULL || page_index >= object->page_count || object->radix_depth == 0u ||
	    object->radix_depth > MEMORY_OBJECT_RADIX_MAX_DEPTH || (phys & (VMM_PAGE_SIZE - 1u)) != 0u || phys == 0u)
		return false;
	indexes_for(object, page_index, indexes);
	if (object->backing_root_or_phys != 0u) {
		uintptr_t node_phys = object->backing_root_or_phys;
		for (uint8_t level = 0u; level < object->radix_depth; level++) {
			struct radix_node* node  = node_virt(node_phys);
			uintptr_t*         entry = &node->entries[indexes[level]];
			if (level + 1u == object->radix_depth) {
				if (*entry != 0u) return false;
				*entry = phys;
				return true;
			}
			if (*entry == 0u) {
				attach_parent = node;
				attach_index  = indexes[level];
				missing_level = level + 1u;
				break;
			}
			node_phys = *entry;
		}
	}
	new_count = (size_t)object->radix_depth - missing_level;
	for (size_t i = 0u; i < new_count; i++) {
		if (!alloc_zeroed_page(&new_nodes[i])) {
			discard_nodes(new_nodes, i);
			return false;
		}
	}
	for (size_t i = 0u; i < new_count; i++) {
		size_t level                                     = (size_t)missing_level + i;
		node_virt(new_nodes[i])->entries[indexes[level]] = i + 1u < new_count ? new_nodes[i + 1u] : phys;
	}
	if (attach_parent != NULL) attach_parent->entries[attach_index] = new_nodes[0];
	else object->backing_root_or_phys = new_nodes[0];
	return true;
}

bool memory_object_radix_resolve(struct memory_object* object, size_t page_index, uintptr_t* out_phys) {
	size_t             indexes[MEMORY_OBJECT_RADIX_MAX_DEPTH];
	uintptr_t          new_nodes[MEMORY_OBJECT_RADIX_MAX_DEPTH];
	struct radix_node* attach_parent = NULL;
	size_t             attach_index  = 0u;
	uint8_t            missing_level = 0u;
	size_t             new_count;
	uintptr_t          data_phys = 0u;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || page_index >= object->page_count || object->radix_depth == 0u ||
	    object->radix_depth > MEMORY_OBJECT_RADIX_MAX_DEPTH)
		return false;
	indexes_for(object, page_index, indexes);
	if (object->backing_root_or_phys != 0u) {
		uintptr_t node_phys = object->backing_root_or_phys;
		for (uint8_t level = 0u; level < object->radix_depth; level++) {
			struct radix_node* node  = node_virt(node_phys);
			uintptr_t*         entry = &node->entries[indexes[level]];
			if (level + 1u == object->radix_depth) {
				if (*entry != 0u) {
					*out_phys = *entry;
					return true;
				}
				if (!alloc_zeroed_page(&data_phys)) return false;
				*entry    = data_phys;
				*out_phys = data_phys;
				return true;
			}
			if (*entry == 0u) {
				attach_parent = node;
				attach_index  = indexes[level];
				missing_level = level + 1u;
				break;
			}
			node_phys = *entry;
		}
	}
	new_count = (size_t)object->radix_depth - missing_level;
	for (size_t i = 0u; i < new_count; i++) {
		if (!alloc_zeroed_page(&new_nodes[i])) {
			discard_nodes(new_nodes, i);
			return false;
		}
	}
	if (!alloc_zeroed_page(&data_phys)) {
		discard_nodes(new_nodes, new_count);
		return false;
	}
	for (size_t i = 0u; i < new_count; i++) {
		size_t level                                     = (size_t)missing_level + i;
		node_virt(new_nodes[i])->entries[indexes[level]] = i + 1u < new_count ? new_nodes[i + 1u] : data_phys;
	}
	if (attach_parent != NULL) attach_parent->entries[attach_index] = new_nodes[0];
	else object->backing_root_or_phys = new_nodes[0];
	*out_phys = data_phys;
	return true;
}

static void release_node(uintptr_t node_phys, uint8_t level, uint8_t depth) {
	struct radix_node* node = node_virt(node_phys);
	for (size_t i = 0u; i < MEMORY_OBJECT_RADIX_ENTRIES; i++) {
		if (node->entries[i] == 0u) continue;
		if (level + 1u == depth)
			(void)pmm_free((struct pmm_extent){.address = node->entries[i], .size = VMM_PAGE_SIZE});
		else release_node(node->entries[i], level + 1u, depth);
	}
	(void)pmm_free((struct pmm_extent){.address = node_phys, .size = VMM_PAGE_SIZE});
}

void memory_object_radix_release(struct memory_object* object) {
	if (object == NULL || object->backing_root_or_phys == 0u) return;
	release_node(object->backing_root_or_phys, 0u, object->radix_depth);
	object->backing_root_or_phys = 0u;
}
