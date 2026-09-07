#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory_backing_rb_node {
	struct memory_backing_rb_node* parent;
	struct memory_backing_rb_node* left;
	struct memory_backing_rb_node* right;
	bool                           red;
};

struct memory_backing_rb_tree {
	struct memory_backing_rb_node* root;
};

enum memory_backing_extent_source {
	MEMORY_BACKING_EXTENT_PMM = 0,
	MEMORY_BACKING_EXTENT_EXTERNAL,
};

struct memory_backing_extent {
	size_t                            logical_start;
	size_t                            size;
	uintptr_t                         physical_start;
	enum memory_backing_extent_source source;
	struct memory_backing_rb_node     tree_node;
	struct memory_backing_extent*     pending_next;
};

/* Insert a linked node and restore red-black tree invariants. */
void memory_backing_rb_insert(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node,
                              struct memory_backing_rb_node* parent, struct memory_backing_rb_node** link);

/* Remove a linked node and restore red-black tree invariants. */
void memory_backing_rb_remove(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node);

/* Return the first node in tree order. */
struct memory_backing_rb_node* memory_backing_rb_first(const struct memory_backing_rb_tree* tree);

/* Return the next node in tree order. */
struct memory_backing_rb_node* memory_backing_rb_next(const struct memory_backing_rb_node* node);

/* Return the previous node in tree order. */
struct memory_backing_rb_node* memory_backing_rb_previous(const struct memory_backing_rb_node* node);

/* Find the extent containing one logical byte offset. */
struct memory_backing_extent* memory_backing_tree_find(const struct memory_backing_rb_tree* tree, size_t offset);

/* Find the first extent beginning at or after a logical byte offset. */
struct memory_backing_extent* memory_backing_tree_lower_bound(const struct memory_backing_rb_tree* tree,
                                                              size_t                               logical_start);

/* Find the first extent intersecting a logical byte range. */
struct memory_backing_extent* memory_backing_tree_first_intersecting(const struct memory_backing_rb_tree* tree,
                                                                     size_t offset, size_t size);

/* Return the preceding extent in logical order. */
struct memory_backing_extent* memory_backing_tree_previous(const struct memory_backing_extent* extent);

/* Return the following extent in logical order. */
struct memory_backing_extent* memory_backing_tree_next(const struct memory_backing_extent* extent);

/* Insert one non-overlapping extent. */
bool memory_backing_tree_insert(struct memory_backing_rb_tree* tree, struct memory_backing_extent* extent);

/* Remove one linked extent. */
void memory_backing_tree_remove(struct memory_backing_rb_tree* tree, struct memory_backing_extent* extent);

/* Split the containing extent at an interior logical byte offset. */
bool memory_backing_tree_split_at(struct memory_backing_rb_tree* tree, size_t offset,
                                  struct memory_backing_extent* right_extent);

/* Validate ordering and all red-black tree invariants. */
bool memory_backing_tree_valid(const struct memory_backing_rb_tree* tree);
