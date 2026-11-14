#include "node.h"

#include <stdio.h>
#include <stdlib.h>

#include "util.h"

uint32_t *leaf_node_num_cells(void *node) {
  return node_at(node, LEAF_NODE_NUM_CELLS_OFFSET);
}

void *leaf_node_cell(void *node, uint32_t cell_num) {
  return node_at(node, LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE);
}

uint32_t *leaf_node_key(void *node, uint32_t cell_num) {
  return leaf_node_cell(node, cell_num);
}

void *leaf_node_value(void *node, uint32_t cell_num) {
  return node_at(leaf_node_cell(node, cell_num), LEAF_NODE_KEY_SIZE);
}

uint32_t *leaf_node_next_leaf(void *node) {
  return node_at(node, LEAF_NODE_NEXT_LEAF_OFFSET);
}

uint32_t *internal_node_num_keys(void *node) {
  return node_at(node, INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t *internal_node_right_child(void *node) {
  return node_at(node, INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t *internal_node_cell(void *node, uint32_t cell_num) {
  return node_at(node, INTERNAL_NODE_HEADER_SIZE +
                           cell_num * INTERNAL_NODE_CELL_SIZE);
}

uint32_t *node_parent(void *node) {
  return node_at(node, PARENT_POINTER_OFFSET);
}

uint32_t *internal_node_child(void *node, uint32_t child_num) {
  uint32_t num_keys = *internal_node_num_keys(node);
  if (child_num > num_keys) {
    printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
    exit(EXIT_FAILURE);
  } else if (child_num == num_keys) {
    uint32_t *right_child = internal_node_right_child(node);
    if (*right_child == INVALID_PAGE_NUM) {
      printf("Tried to access right child of node,but was invalid page\n");
      exit(EXIT_FAILURE);
    }
    return right_child;
  } else {
    uint32_t *child = internal_node_cell(node, child_num);
    if (*child == INVALID_PAGE_NUM) {
      printf("Tried to access child %d of node,but was invalid page\n",
             child_num);
      exit(EXIT_FAILURE);
    }
    return child;
  }
}

uint32_t *internal_node_key(void *node, uint32_t key_num) {
  return node_at(internal_node_cell(node, key_num), INTERNAL_NODE_CHILD_SIZE);
}

NodeType get_node_type(void *node) {
  uint8_t value = *((uint8_t *)node_at(node, NODE_TYPE_OFFSET));
  return (NodeType)value;
}

void set_node_type(void *node, NodeType type) {
  uint8_t value = type;
  *((uint8_t *)node_at(node, NODE_TYPE_OFFSET)) = value;
}

void set_node_root(void *node, bool is_root) {
  uint8_t value = is_root;
  *((uint8_t *)node_at(node, IS_ROOT_OFFSET)) = value;
}

void init_leaf_node(void *node) {
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
  *leaf_node_next_leaf(node) = NO_NEXT_LEAF;
}

void init_internal_node(void *node) {
  set_node_type(node, NODE_INTERNAL);
  set_node_root(node, false);
  *internal_node_num_keys(node) = 0;

  // Necessary because the root page number is 0; by not initializing an
  // internal node's right child to an invalid page number when initializing the
  // node, we may end up with 0 as the node's right child, which makes the node
  // a parent of the root
  *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

bool is_node_root(void *node) {
  uint8_t value = *((uint8_t *)node_at(node, IS_ROOT_OFFSET));
  return (bool)value;
}

uint32_t get_node_max_key(Pager *pager, void *node) {
  if (get_node_type(node) == NODE_LEAF) {
    return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
  }
  void *right_child = get_page(pager, *internal_node_right_child(node));
  return get_node_max_key(pager, right_child);
}

// ============================================================================
// Debug output: .constants and .btree
// ============================================================================

static void indent(uint32_t level) {
  for (uint32_t i = 0; i < level; i++) {
    printf("  ");
  }
}

void print_constants(void) {
  printf("ROW_SIZE: %d\n", ROW_SIZE);
  printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
  printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
  printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE);
  printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
  printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS);
}

void print_tree(Pager *pager, uint32_t page_num, uint32_t indentation_level) {
  void *node = get_page(pager, page_num);
  uint32_t num_keys, child;
  switch (get_node_type(node)) {
  case NODE_INTERNAL:
    num_keys = *internal_node_num_keys(node);
    indent(indentation_level);
    printf("- internal (size %d)\n", num_keys);
    for (uint32_t i = 0; i < num_keys; i++) {
      child = *internal_node_child(node, i);
      print_tree(pager, child, indentation_level + 1);

      indent(indentation_level + 1);
      printf("- key %d\n", *internal_node_key(node, i));
    }
    // An empty internal node has no right child to print.
    child = *internal_node_right_child(node);
    if (child != INVALID_PAGE_NUM) {
      print_tree(pager, child, indentation_level + 1);
    }
    break;
  case NODE_LEAF:
    num_keys = *leaf_node_num_cells(node);
    indent(indentation_level);
    printf("- leaf (size %d)\n", num_keys);
    for (uint32_t i = 0; i < num_keys; i++) {
      indent(indentation_level + 1);
      printf("- %d\n", *leaf_node_key(node, i));
    }
    break;
  }
}
