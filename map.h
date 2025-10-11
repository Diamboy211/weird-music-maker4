#include <stdlib.h>
#include <stdint.h>

#define declare_map(name, node_name, key_type, value_type) \
	typedef struct name name; \
	typedef struct node_name node_name;

#define define_map(name, node_name, key_type, value_type, def) \
	declare_map(name, node_name, key_type, value_type) \
	typedef struct node_name \
	{ \
		value_type value; \
		struct node_name *parent; \
		union \
		{ \
			struct { struct node_name *left, *right; }; \
			struct node_name *child[2]; \
		}; \
		key_type key; \
		uint8_t red : 1; \
		uint8_t dir : 1; \
	} node_name; \
	struct name \
	{ \
		node_name *root; \
	}; \
	static void name##rotate(name *tree, node_name *node, uint8_t dir) \
	{ \
		node_name *parent = node->parent; \
		node_name *root = node->child[1 - dir]; \
		node_name *child = root->child[dir]; \
		node->child[1 - dir] = child; \
		if (child != NULL) \
		{ \
			child->dir = 1 - dir; \
			child->parent = node; \
		} \
		root->child[dir] = node; \
		node->parent = root; \
		root->dir = node->dir; \
		node->dir = dir; \
		root->parent = parent; \
		if (parent != NULL) parent->child[root->dir] = root; \
		else tree->root = root; \
	} \
	name name##_create() \
	{ \
		return (name){0}; \
	} \
	value_type *name##_get(name *tree, key_type key) \
	{ \
		node_name *it1 = NULL, *it2 = tree->root; \
		uint8_t dir = 0; \
		while (it2 != NULL) \
		{ \
			if (key == it2->key) return &it2->value; \
			it1 = it2; \
			if (key < it2->key) it2 = it2->left, dir = 0; \
			else it2 = it2->right, dir = 1; \
		} \
		node_name *node = calloc(1, sizeof(node_name)); \
		if (node == NULL) return NULL; \
		node->key = key; \
		node->value = def; \
		node->red = 1; \
		if (it1 == NULL) \
		{ \
			tree->root = node; \
			return &node->value; \
		} \
		node->parent = it1; \
		node->dir = dir; \
		it1->child[dir] = node; \
		it2 = node; \
		for (;;) \
		{ \
			if (it1 == NULL || it1->red == 0) return &node->value; \
			node_name *it0 = it1->parent; \
			if (it0 == NULL) \
			{ \
				it1->red = 0; \
				return &node->value; \
			} \
			uint8_t dir = it1->dir; \
			node_name *iu = it0->child[1 - dir]; \
			if (iu == NULL || iu->red == 0) \
			{ \
				if (it2->dir != dir) \
				{ \
					name##rotate(tree, it1, dir); \
					it2 = it1; \
					it1 = it0->child[dir]; \
				} \
				name##rotate(tree, it0, 1 - dir); \
				it0->red = 1; \
				it1->red = 0; \
				return &node->value; \
			} \
			it1->red = 0; \
			iu->red = 0; \
			it0->red = 1; \
			it2 = it0; \
			it1 = it2->parent; \
		} \
	} \
	void name##_destroy(name *tree) \
	{ \
		node_name *it = tree->root; \
		tree->root = NULL; \
		while (it) \
		{ \
			if (it->left != NULL) it = it->left; \
			else if (it->right != NULL) it = it->right; \
			else \
			{ \
				node_name *p = it->parent; \
				uint8_t dir = it->dir; \
				free(it); \
				if (p == NULL) return; \
				it = p; \
				p->child[dir] = NULL; \
			} \
		} \
	} \
	node_name *name##_begin(name *tree) \
	{ \
		node_name *it = tree->root; \
		if (it == NULL) return NULL; \
		while (it->left != NULL) it = it->left; \
		return it; \
	} \
	node_name *name##_next(node_name *node) \
	{ \
		if (node->right == NULL) \
		{ \
			for (;;) \
			{ \
				if (node == NULL) return NULL; \
				if (node->dir == 0) return node->parent; \
				node = node->parent; \
			} \
		} \
		node = node->right; \
		while (node->left) node = node->left; \
		return node; \
	}
