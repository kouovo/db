#ifndef DB_NODE_H
#define DB_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "pager.h"
#include "row.h"

// B-tree nodes: page layout and accessors

#define INVALID_PAGE_NUM UINT32_MAX // internal node: no child here
#define NO_NEXT_LEAF 0 // leaf: rightmost leaf (page 0 is always the root)

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

/*
 * Common Node Header Layout
 */
#define NODE_TYPE_SIZE ((uint32_t)sizeof(uint8_t))
#define NODE_TYPE_OFFSET ((uint32_t)0)
#define IS_ROOT_SIZE ((uint32_t)sizeof(uint8_t))
#define IS_ROOT_OFFSET (NODE_TYPE_SIZE)
#define PARENT_POINTER_SIZE ((uint32_t)sizeof(uint32_t))
#define PARENT_POINTER_OFFSET (IS_ROOT_OFFSET + IS_ROOT_SIZE)
#define COMMON_NODE_HEADER_SIZE                                                \
  (NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE)

/*
 * Leaf Node Header Layout
 */
#define LEAF_NODE_NUM_CELLS_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_NUM_CELLS_OFFSET (COMMON_NODE_HEADER_SIZE)
#define LEAF_NODE_NEXT_LEAF_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_NEXT_LEAF_OFFSET                                             \
  (LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE)
#define LEAF_NODE_HEADER_SIZE                                                  \
  (COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE +                        \
   LEAF_NODE_NEXT_LEAF_SIZE)

/*
 * Leaf Node Body Layout
 */
#define LEAF_NODE_KEY_SIZE ((uint32_t)sizeof(uint32_t))
#define LEAF_NODE_KEY_OFFSET ((uint32_t)0)
#define LEAF_NODE_VALUE_SIZE (ROW_SIZE)
#define LEAF_NODE_VALUE_OFFSET (LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE)
#define LEAF_NODE_CELL_SIZE (LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE)
#define LEAF_NODE_SPACE_FOR_CELLS (PAGE_SIZE - LEAF_NODE_HEADER_SIZE)
#define LEAF_NODE_MAX_CELLS (LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE)
#define LEAF_NODE_RIGHT_SPLIT_COUNT (uint32_t)((LEAF_NODE_MAX_CELLS + 1) / 2)
#define LEAF_NODE_LEFT_SPLIT_COUNT                                             \
  (uint32_t)((LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT)

/*
 * Internal Node Header Layout
 */
#define INTERNAL_NODE_NUM_KEYS_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_NUM_KEYS_OFFSET (COMMON_NODE_HEADER_SIZE)
#define INTERNAL_NODE_RIGHT_CHILD_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET                                       \
  (INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE)
#define INTERNAL_NODE_HEADER_SIZE                                              \
  (COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE +                     \
   INTERNAL_NODE_RIGHT_CHILD_SIZE)
/*
 * Internal Node Body Layout
 */
#define INTERNAL_NODE_KEY_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_CHILD_SIZE ((uint32_t)sizeof(uint32_t))
#define INTERNAL_NODE_CELL_SIZE                                                \
  (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_MAX_CELLS ((uint32_t)3)

/*
 * Internal node page layout (PAGE_SIZE = 4096 bytes)
 *
 *  offset
 *       0 +-----------------------------+ <- node
 *         | node_type          (1 byte) |
 *       1 +-----------------------------+
 *         | is_root            (1 byte) |  common header (6)
 *       2 +-----------------------------+
 *         | parent_pointer     (4 bytes)|
 *       6 +-----------------------------+ <- INTERNAL_NODE_NUM_KEYS_OFFSET
 *         | num_keys           (4 bytes)|
 *      10 +-----------------------------+ <- INTERNAL_NODE_RIGHT_CHILD_OFFSET
 *         | right_child        (4 bytes)|  internal header (14 total)
 *      14 +-----------------------------+ <- cell 0
 *         | cell 0             (8 bytes)|
 *      22 +-----------------------------+ <- cell 1
 *         | cell 1             (8 bytes)|
 *      30 +-----------------------------+
 *         | ...                         |
 *    4086 +-----------------------------+ <- cell 509
 *         | cell 509           (8 bytes)|  (4096 - 14) / 8 = 510 cells fit
 *    4094 +-----------------------------+
 *         | wasted space       (2 bytes)|
 *    4096 +-----------------------------+
 *
 * One cell (INTERNAL_NODE_CELL_SIZE = 8 bytes)
 *
 *       0 +-----------------------------+
 *         | child (page num)   (4 bytes)|
 *       4 +-----------------------------+
 *         | key                (4 bytes)|  max key in that child
 *       8 +-----------------------------+
 *
 * Tree view (num_keys = N):
 *
 *            [ child0 | key0 | child1 | key1 | ... | keyN-1 | right_child ]
 *               /              |                               \
 *         keys <= key0   key0 < keys <= key1          keys > keyN-1
 */

/*
 * Leaf node page layout (PAGE_SIZE = 4096 bytes)
 *
 *  offset
 *       0 +-----------------------------+ <- node
 *         | node_type          (1 byte) |
 *       1 +-----------------------------+
 *         | is_root            (1 byte) |  common header (6)
 *       2 +-----------------------------+
 *         | parent_pointer     (4 bytes)|
 *       6 +-----------------------------+ <- leaf_node_num_cells(node)
 *         | num_cells          (4 bytes)|
 *      10 +-----------------------------+ <- leaf_node_next_leaf(node)
 *         | next_leaf          (4 bytes)|  leaf header (14 total)
 *      14 +-----------------------------+ <- leaf_node_cell(node, 0)
 *         | cell 0           (297 bytes)|     = leaf_node_key(node, 0)
 *     311 +-----------------------------+ <- leaf_node_cell(node, 1)
 *         | cell 1           (297 bytes)|
 *     608 +-----------------------------+
 *         | ...                         |
 *    3578 +-----------------------------+ <- leaf_node_cell(node, 12)
 *         | cell 12          (297 bytes)|  LEAF_NODE_MAX_CELLS = 13
 *    3875 +-----------------------------+
 *         | wasted space     (221 bytes)|
 *    4096 +-----------------------------+
 *
 * next_leaf is the page number of the right sibling leaf, so leaves form a
 * chain in key order. 0 means no sibling: page 0 is always the root, so it
 * can never be anyone's right sibling.
 *
 * One cell (LEAF_NODE_CELL_SIZE = 297 bytes)
 *
 *       0 +-----------------------------+ <- leaf_node_key(node, n)
 *         | key                (4 bytes)|
 *       4 +-----------------------------+ <- leaf_node_value(node, n)
 *         | id                 (4 bytes)|
 *       8 +-----------------------------+
 *         | username          (33 bytes)|  value = serialized Row (293)
 *      41 +-----------------------------+
 *         | email            (256 bytes)|
 *     297 +-----------------------------+
 */
uint32_t *leaf_node_num_cells(void *node);
void *leaf_node_cell(void *node, uint32_t cell_num);
uint32_t *leaf_node_key(void *node, uint32_t cell_num);
void *leaf_node_value(void *node, uint32_t cell_num);
uint32_t *leaf_node_next_leaf(void *node);

uint32_t *internal_node_num_keys(void *node);
uint32_t *internal_node_right_child(void *node);
uint32_t *internal_node_cell(void *node, uint32_t cell_num);
uint32_t *internal_node_child(void *node, uint32_t child_num);
uint32_t *internal_node_key(void *node, uint32_t key_num);

uint32_t *node_parent(void *node);
NodeType get_node_type(void *node);
void set_node_type(void *node, NodeType type);
void set_node_root(void *node, bool is_root);
bool is_node_root(void *node);
void init_leaf_node(void *node);
void init_internal_node(void *node);
uint32_t get_node_max_key(Pager *pager, void *node);

// Debug output: .constants and .btree
void print_constants(void);
void print_tree(Pager *pager, uint32_t page_num, uint32_t indentation_level);

#endif
