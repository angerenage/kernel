#include "memory_backing_tree.h"

#include <stddef.h>

#define EXTENT_FROM_NODE(node)                                                                                         \
	((node) == NULL                                                                                                    \
	     ? NULL                                                                                                        \
	     : (struct memory_backing_extent*)((uint8_t*)(node) - offsetof(struct memory_backing_extent, tree_node)))

static bool node_is_red(const struct memory_backing_rb_node* node) {
	return node != NULL && node->red;
}

static bool node_is_black(const struct memory_backing_rb_node* node) {
	return !node_is_red(node);
}

static void rotate_left(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node) {
	struct memory_backing_rb_node* right = node->right;
	node->right                          = right->left;
	if (right->left != NULL) right->left->parent = node;
	right->parent = node->parent;
	if (node->parent == NULL) tree->root = right;
	else if (node == node->parent->left) node->parent->left = right;
	else node->parent->right = right;
	right->left  = node;
	node->parent = right;
}

static void rotate_right(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node) {
	struct memory_backing_rb_node* left = node->left;
	node->left                          = left->right;
	if (left->right != NULL) left->right->parent = node;
	left->parent = node->parent;
	if (node->parent == NULL) tree->root = left;
	else if (node == node->parent->right) node->parent->right = left;
	else node->parent->left = left;
	left->right  = node;
	node->parent = left;
}

void memory_backing_rb_insert(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node,
                              struct memory_backing_rb_node* parent, struct memory_backing_rb_node** link) {
	node->parent = parent;
	node->left   = NULL;
	node->right  = NULL;
	node->red    = true;
	*link        = node;
	while (node != tree->root && node_is_red(node->parent)) {
		struct memory_backing_rb_node* grandparent = node->parent->parent;
		if (node->parent == grandparent->left) {
			struct memory_backing_rb_node* uncle = grandparent->right;
			if (node_is_red(uncle)) {
				node->parent->red = false;
				uncle->red        = false;
				grandparent->red  = true;
				node              = grandparent;
			}
			else {
				if (node == node->parent->right) {
					node = node->parent;
					rotate_left(tree, node);
				}
				node->parent->red = false;
				grandparent->red  = true;
				rotate_right(tree, grandparent);
			}
		}
		else {
			struct memory_backing_rb_node* uncle = grandparent->left;
			if (node_is_red(uncle)) {
				node->parent->red = false;
				uncle->red        = false;
				grandparent->red  = true;
				node              = grandparent;
			}
			else {
				if (node == node->parent->left) {
					node = node->parent;
					rotate_right(tree, node);
				}
				node->parent->red = false;
				grandparent->red  = true;
				rotate_left(tree, grandparent);
			}
		}
	}
	tree->root->red = false;
}

static struct memory_backing_rb_node* subtree_minimum(struct memory_backing_rb_node* node) {
	while (node != NULL && node->left != NULL) node = node->left;
	return node;
}

static void transplant(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* old_node,
                       struct memory_backing_rb_node* new_node) {
	if (old_node->parent == NULL) tree->root = new_node;
	else if (old_node == old_node->parent->left) old_node->parent->left = new_node;
	else old_node->parent->right = new_node;
	if (new_node != NULL) new_node->parent = old_node->parent;
}

static void remove_fixup(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node,
                         struct memory_backing_rb_node* parent) {
	while (node != tree->root && node_is_black(node)) {
		struct memory_backing_rb_node* sibling;
		if (parent == NULL) break;
		if (node == parent->left) {
			sibling = parent->right;
			if (node_is_red(sibling)) {
				sibling->red = false;
				parent->red  = true;
				rotate_left(tree, parent);
				sibling = parent->right;
			}
			if (sibling == NULL) {
				node   = parent;
				parent = node->parent;
				continue;
			}
			if (node_is_black(sibling->left) && node_is_black(sibling->right)) {
				sibling->red = true;
				node         = parent;
				parent       = node->parent;
			}
			else {
				if (node_is_black(sibling->right)) {
					if (sibling->left != NULL) sibling->left->red = false;
					sibling->red = true;
					rotate_right(tree, sibling);
					sibling = parent->right;
				}
				sibling->red = parent->red;
				parent->red  = false;
				if (sibling->right != NULL) sibling->right->red = false;
				rotate_left(tree, parent);
				node   = tree->root;
				parent = NULL;
			}
		}
		else {
			sibling = parent->left;
			if (node_is_red(sibling)) {
				sibling->red = false;
				parent->red  = true;
				rotate_right(tree, parent);
				sibling = parent->left;
			}
			if (sibling == NULL) {
				node   = parent;
				parent = node->parent;
				continue;
			}
			if (node_is_black(sibling->right) && node_is_black(sibling->left)) {
				sibling->red = true;
				node         = parent;
				parent       = node->parent;
			}
			else {
				if (node_is_black(sibling->left)) {
					if (sibling->right != NULL) sibling->right->red = false;
					sibling->red = true;
					rotate_left(tree, sibling);
					sibling = parent->left;
				}
				sibling->red = parent->red;
				parent->red  = false;
				if (sibling->left != NULL) sibling->left->red = false;
				rotate_right(tree, parent);
				node   = tree->root;
				parent = NULL;
			}
		}
	}
	if (node != NULL) node->red = false;
}

void memory_backing_rb_remove(struct memory_backing_rb_tree* tree, struct memory_backing_rb_node* node) {
	struct memory_backing_rb_node* moved       = node;
	struct memory_backing_rb_node* replacement = NULL;
	struct memory_backing_rb_node* parent      = NULL;
	bool                           removed_red = moved->red;
	if (node->left == NULL) {
		replacement = node->right;
		parent      = node->parent;
		transplant(tree, node, node->right);
	}
	else if (node->right == NULL) {
		replacement = node->left;
		parent      = node->parent;
		transplant(tree, node, node->left);
	}
	else {
		moved       = subtree_minimum(node->right);
		removed_red = moved->red;
		replacement = moved->right;
		if (moved->parent == node) {
			parent = moved;
			if (replacement != NULL) replacement->parent = moved;
		}
		else {
			parent = moved->parent;
			transplant(tree, moved, moved->right);
			moved->right         = node->right;
			moved->right->parent = moved;
		}
		transplant(tree, node, moved);
		moved->left         = node->left;
		moved->left->parent = moved;
		moved->red          = node->red;
	}
	if (!removed_red) remove_fixup(tree, replacement, parent);
	node->parent = NULL;
	node->left   = NULL;
	node->right  = NULL;
	node->red    = false;
}

struct memory_backing_rb_node* memory_backing_rb_first(const struct memory_backing_rb_tree* tree) {
	return subtree_minimum(tree->root);
}

struct memory_backing_rb_node* memory_backing_rb_next(const struct memory_backing_rb_node* node) {
	const struct memory_backing_rb_node* current;
	if (node == NULL) return NULL;
	if (node->right != NULL) return subtree_minimum(node->right);
	current = node->parent;
	while (current != NULL && node == current->right) {
		node    = current;
		current = current->parent;
	}
	return (struct memory_backing_rb_node*)current;
}

struct memory_backing_rb_node* memory_backing_rb_previous(const struct memory_backing_rb_node* node) {
	const struct memory_backing_rb_node* current;
	if (node == NULL) return NULL;
	if (node->left != NULL) {
		current = node->left;
		while (current->right != NULL) current = current->right;
		return (struct memory_backing_rb_node*)current;
	}
	current = node->parent;
	while (current != NULL && node == current->left) {
		node    = current;
		current = current->parent;
	}
	return (struct memory_backing_rb_node*)current;
}

struct memory_backing_extent* memory_backing_tree_find(const struct memory_backing_rb_tree* tree, size_t offset) {
	struct memory_backing_rb_node* node      = tree->root;
	struct memory_backing_extent*  candidate = NULL;
	while (node != NULL) {
		struct memory_backing_extent* extent = EXTENT_FROM_NODE(node);
		if (offset < extent->logical_start) node = node->left;
		else {
			candidate = extent;
			node      = node->right;
		}
	}
	if (candidate != NULL && offset - candidate->logical_start < candidate->size) return candidate;
	return NULL;
}

struct memory_backing_extent* memory_backing_tree_lower_bound(const struct memory_backing_rb_tree* tree,
                                                              size_t                               logical_start) {
	struct memory_backing_rb_node* node      = tree->root;
	struct memory_backing_extent*  candidate = NULL;
	while (node != NULL) {
		struct memory_backing_extent* extent = EXTENT_FROM_NODE(node);
		if (extent->logical_start >= logical_start) {
			candidate = extent;
			node      = node->left;
		}
		else node = node->right;
	}
	return candidate;
}

struct memory_backing_extent* memory_backing_tree_first_intersecting(const struct memory_backing_rb_tree* tree,
                                                                     size_t offset, size_t size) {
	struct memory_backing_extent* extent;
	if (size == 0u) return NULL;
	extent = memory_backing_tree_find(tree, offset);
	if (extent != NULL) return extent;
	extent = memory_backing_tree_lower_bound(tree, offset);
	return extent != NULL && extent->logical_start - offset < size ? extent : NULL;
}

struct memory_backing_extent* memory_backing_tree_previous(const struct memory_backing_extent* extent) {
	return extent == NULL ? NULL : EXTENT_FROM_NODE(memory_backing_rb_previous(&extent->tree_node));
}

struct memory_backing_extent* memory_backing_tree_next(const struct memory_backing_extent* extent) {
	return extent == NULL ? NULL : EXTENT_FROM_NODE(memory_backing_rb_next(&extent->tree_node));
}

bool memory_backing_tree_insert(struct memory_backing_rb_tree* tree, struct memory_backing_extent* extent) {
	struct memory_backing_rb_node*  parent = NULL;
	struct memory_backing_rb_node** link   = &tree->root;
	struct memory_backing_extent*   before = NULL;
	struct memory_backing_extent*   after  = NULL;
	if (tree == NULL || extent == NULL || extent->size == 0u || extent->logical_start > SIZE_MAX - extent->size)
		return false;
	while (*link != NULL) {
		struct memory_backing_extent* current = EXTENT_FROM_NODE(*link);
		parent                                = *link;
		if (extent->logical_start < current->logical_start) {
			after = current;
			link  = &(*link)->left;
		}
		else if (extent->logical_start > current->logical_start) {
			before = current;
			link   = &(*link)->right;
		}
		else return false;
	}
	if ((before != NULL && before->size > extent->logical_start - before->logical_start) ||
	    (after != NULL && extent->size > after->logical_start - extent->logical_start))
		return false;
	memory_backing_rb_insert(tree, &extent->tree_node, parent, link);
	return true;
}

void memory_backing_tree_remove(struct memory_backing_rb_tree* tree, struct memory_backing_extent* extent) {
	memory_backing_rb_remove(tree, &extent->tree_node);
}

bool memory_backing_tree_split_at(struct memory_backing_rb_tree* tree, size_t offset,
                                  struct memory_backing_extent* right_extent) {
	struct memory_backing_extent* extent = memory_backing_tree_find(tree, offset);
	size_t                        left_size;
	if (extent == NULL || right_extent == NULL || offset == extent->logical_start) return false;
	left_size     = offset - extent->logical_start;
	*right_extent = (struct memory_backing_extent){
		.logical_start  = offset,
		.size           = extent->size - left_size,
		.physical_start = extent->physical_start + left_size,
		.source         = extent->source,
	};
	extent->size = left_size;
	if (memory_backing_tree_insert(tree, right_extent)) return true;
	extent->size += right_extent->size;
	return false;
}

static bool validate_node(const struct memory_backing_rb_node* node, const struct memory_backing_rb_node* parent,
                          size_t* previous_end, bool* have_previous, unsigned black_depth,
                          unsigned* expected_black_depth) {
	const struct memory_backing_extent* extent;
	if (node == NULL) {
		if (*expected_black_depth == 0u) *expected_black_depth = black_depth;
		return *expected_black_depth == black_depth;
	}
	if (node->parent != parent || (node->red && (node_is_red(node->left) || node_is_red(node->right)))) return false;
	if (!node->red) black_depth++;
	if (!validate_node(node->left, node, previous_end, have_previous, black_depth, expected_black_depth)) return false;
	extent = EXTENT_FROM_NODE(node);
	if (extent->size == 0u || extent->logical_start > SIZE_MAX - extent->size ||
	    (*have_previous && *previous_end > extent->logical_start))
		return false;
	*previous_end  = extent->logical_start + extent->size;
	*have_previous = true;
	return validate_node(node->right, node, previous_end, have_previous, black_depth, expected_black_depth);
}

bool memory_backing_tree_valid(const struct memory_backing_rb_tree* tree) {
	size_t   previous_end         = 0u;
	bool     have_previous        = false;
	unsigned expected_black_depth = 0u;
	if (tree == NULL || (tree->root != NULL && (tree->root->parent != NULL || tree->root->red))) return false;
	return validate_node(tree->root, NULL, &previous_end, &have_previous, 0u, &expected_black_depth);
}
