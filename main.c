#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255
#define TABLE_MAX_PAGES 100
#define INVALID_PAGE_NUM UINT32_MAX // internal node: no child here
#define NO_NEXT_LEAF 0 // leaf: rightmost leaf (page 0 is always the root)

// Address of the field `offset` bytes into a page (or a serialized row).
// Arithmetic on void * is a GNU extension, so go through char *.
void *node_at(void *node, uint32_t offset) { return (char *)node + offset; }

// ============================================================================
// Rows: in-memory Row and its on-page layout
// ============================================================================

#define size_of_attribute(Struct, Attribute) sizeof(((Struct *)0)->Attribute)
typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

#define ID_SIZE ((uint32_t)size_of_attribute(Row, id))
#define USERNAME_SIZE ((uint32_t)size_of_attribute(Row, username))
#define EMAIL_SIZE ((uint32_t)size_of_attribute(Row, email))
#define ID_OFFSET ((uint32_t)0)
#define USERNAME_OFFSET (ID_OFFSET + ID_SIZE)
#define EMAIL_OFFSET (USERNAME_OFFSET + USERNAME_SIZE)
#define ROW_SIZE (ID_SIZE + USERNAME_SIZE + EMAIL_SIZE)
#define PAGE_SIZE ((uint32_t)4096)

void serialize_row(Row *source, void *destination) {
  memcpy(node_at(destination, ID_OFFSET), &(source->id), ID_SIZE);
  memcpy(node_at(destination, USERNAME_OFFSET), &(source->username),
         USERNAME_SIZE);
  memcpy(node_at(destination, EMAIL_OFFSET), &(source->email), EMAIL_SIZE);
}

void deserialize_row(void *source, Row *destination) {
  memcpy(&(destination->id), node_at(source, ID_OFFSET), ID_SIZE);
  memcpy(&(destination->username), node_at(source, USERNAME_OFFSET),
         USERNAME_SIZE);
  memcpy(&(destination->email), node_at(source, EMAIL_OFFSET), EMAIL_SIZE);
}

void print_row(Row *row) {
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

// ============================================================================
// Pager: cache of 4 KB file pages
// ============================================================================

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  void *pages[TABLE_MAX_PAGES];
  uint32_t num_pages;
} Pager;

typedef struct {
  Pager *pager;
  uint32_t root_page_num;
} Table;

typedef struct {
  Table *table;
  uint32_t page_num;
  uint32_t cell_num;
  bool end_of_table;
} Cursor;

void *get_page(Pager *pager, uint32_t page_num) {
  if (page_num >= TABLE_MAX_PAGES) {
    printf("Tried to fetch page number out of bounds. %d >= %d\n", page_num,
           TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager->pages[page_num] == NULL) {
    // Cache miss.
    void *page = calloc(1, PAGE_SIZE);
    uint32_t num_pages = pager->file_length / PAGE_SIZE;

    // Might save a partial page at the end of the file.
    if (pager->file_length % PAGE_SIZE) {
      num_pages += 1;
    }

    if (page_num <= num_pages) {
      lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
      ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
      if (bytes_read == -1) {
        printf("Error reading file: %d\n", errno);
        exit(EXIT_FAILURE);
      }
    }
    pager->pages[page_num] = page;

    if (page_num >= pager->num_pages) {
      pager->num_pages = page_num + 1;
    }
  }
  return pager->pages[page_num];
}

// until we start recycling free pages, new pages will always
// go onto the end of databse file.
uint32_t get_unused_page_num(Pager *pager) { return pager->num_pages; }

Pager *pager_open(const char *filename) {
  int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
  if (fd == -1) {
    printf("unable to open file\n");
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  Pager *pager = malloc(sizeof(Pager));
  pager->file_descriptor = fd;
  pager->file_length = file_length;
  pager->num_pages = (file_length / PAGE_SIZE);

  if (file_length % PAGE_SIZE != 0) {
    printf("Db file is not a whole number of pages.Corrupt file.\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    pager->pages[i] = NULL;
  }
  return pager;
}

void pager_flush(Pager *pager, uint32_t page_num) {
  if (pager->pages[page_num] == NULL) {
    printf("Tried to flush null page\n");
    exit(EXIT_FAILURE);
  }

  off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

  if (offset == -1) {
    printf("Error seeking: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  ssize_t bytes_written =
      write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);

  if (bytes_written == -1) {
    printf("Error writing: %d\n", errno);
    exit(EXIT_FAILURE);
  }
}

// ============================================================================
// B-tree nodes: page layout and accessors
// ============================================================================

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
// Search: find where a key lives
// ============================================================================

Cursor *leaf_node_find(Table *table, uint32_t page_num, uint32_t key) {
  void *node = get_page(table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor *cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = page_num;
  uint32_t min_index = 0;
  uint32_t one_past_max_index = num_cells;
  while (one_past_max_index != min_index) {
    uint32_t index = (min_index + one_past_max_index) / 2;
    uint32_t key_at_index = *leaf_node_key(node, index);
    if (key == key_at_index) {
      cursor->cell_num = index;
      return cursor;
    }
    if (key < key_at_index) {
      one_past_max_index = index;
    } else {
      min_index = index + 1;
    }
  }
  cursor->cell_num = min_index;
  return cursor;
}

uint32_t internal_node_find_child(void *node, uint32_t key) {
  // Return the index of the child which should contain the given key.
  uint32_t num_keys = *internal_node_num_keys(node);

  // Binary search: find the first key_i >= key.
  uint32_t min_index = 0;
  uint32_t max_index = num_keys; // there is one more child than key
  while (min_index != max_index) {
    uint32_t index = (min_index + max_index) / 2;
    uint32_t key_to_right = *internal_node_key(node, index);
    if (key_to_right >= key) {
      max_index = index;
    } else {
      min_index = index + 1;
    }
  }
  return min_index;
}

/*
   Return the position of the given key.
   if the key is not present, return the position
   where it should be inserted
   */
Cursor *table_find(Table *table, uint32_t key) {
  // Walk down from the root, following the child that covers key, until we
  // reach the leaf that holds (or should hold) it.
  uint32_t page_num = table->root_page_num;
  void *node = get_page(table->pager, page_num);
  while (get_node_type(node) == NODE_INTERNAL) {
    uint32_t child_index = internal_node_find_child(node, key);
    page_num = *internal_node_child(node, child_index);
    node = get_page(table->pager, page_num);
  }
  return leaf_node_find(table, page_num, key);
}

// ============================================================================
// Insert: add a row, splitting full nodes
// ============================================================================

// Writes children[0..count) into the internal node at page_num, in order:
// every child but the last gets a cell keyed by its max key, and the last
// one becomes right_child. Also points each child's parent at this node.
void internal_node_set_children(Pager *pager, uint32_t page_num,
                                const uint32_t *children, uint32_t count) {
  void *node = get_page(pager, page_num);
  *internal_node_num_keys(node) = count - 1;
  for (uint32_t i = 0; i < count; i++) {
    void *child = get_page(pager, children[i]);
    *node_parent(child) = page_num;
    if (i + 1 < count) {
      // Write through internal_node_cell: internal_node_child refuses cells
      // that do not hold a valid page yet.
      *internal_node_cell(node, i) = children[i];
      *internal_node_key(node, i) = get_node_max_key(pager, child);
    } else {
      *internal_node_right_child(node) = children[i];
    }
  }
}

// Turns the root page into an internal node with exactly two children.
void set_root_children(Table *table, uint32_t left_page_num,
                       uint32_t right_page_num) {
  void *root = get_page(table->pager, table->root_page_num);
  init_internal_node(root);
  set_node_root(root, true);
  uint32_t children[] = {left_page_num, right_page_num};
  internal_node_set_children(table->pager, table->root_page_num, children, 2);
}

/*
 * create_new_root: called when the leaf being split is the root.
 *
 * The root must stay at table->root_page_num (page 0), so the old root's
 * content is moved out to a fresh page, and page 0 is reused as the new
 * internal root.
 *
 *  before (after leaf split, page 0 still marked root):
 *
 *     page 0 (leaf, root)          page 1 (leaf, new_node)
 *    [ 1 2 3 4 5 6 7 ] next=1 --> [ 8 9 10 11 12 13 14 ] next=0
 *
 *  step 1: left_child_page_num = 2, memcpy(page 2, page 0)
 *          (the copy keeps next_leaf = 1, so the chain stays intact)
 *
 *     page 0 (stale copy)   page 2 (leaf)                page 1 (leaf)
 *    [ 1 2 3 4 5 6 7 ]     [ 1 2 3 4 5 6 7 ] next=1 --> [ 8 ... 14 ] next=0
 *
 *  step 2: set_root_children(page 2, page 1) rewrites page 0
 *
 *                 page 0 (internal, root)
 *          +---------------------------------+
 *          | num_keys    = 1                 |
 *          | cell 0      = (child=2, key=7)  |  key = max key of left
 *          | right_child = 1                 |
 *          +---------------------------------+
 *                 /                    \
 *       page 2 (leaf)               page 1 (leaf)
 *     [ 1 2 3 4 5 6 7 ] -------> [ 8 9 10 11 12 13 14 ] -> 0 (end)
 *       keys <= 7       next_leaf    keys > 7
 *
 * A full internal root is split by internal_node_split_and_insert instead.
 */
void create_new_root(Table *table, uint32_t right_child_page_num) {
  void *root = get_page(table->pager, table->root_page_num);
  uint32_t left_child_page_num = get_unused_page_num(table->pager);
  void *left_child = get_page(table->pager, left_child_page_num);
  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);
  set_root_children(table, left_child_page_num, right_child_page_num);
}

void internal_node_split_and_insert(Table *table, uint32_t node_page_num,
                                    uint32_t child_page_num);

// Adds child_page_num to the internal node at parent_page_num, keyed by the
// child's max key. Splits the parent first if it is full.
void internal_node_insert(Table *table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
  void *parent = get_page(table->pager, parent_page_num);
  void *child = get_page(table->pager, child_page_num);
  uint32_t child_max_key = get_node_max_key(table->pager, child);
  uint32_t index = internal_node_find_child(parent, child_max_key);

  uint32_t original_num_keys = *internal_node_num_keys(parent);
  if (original_num_keys >= INTERNAL_NODE_MAX_CELLS) {
    internal_node_split_and_insert(table, parent_page_num, child_page_num);
    return;
  }

  uint32_t right_child_page_num = *internal_node_right_child(parent);
  void *right_child = get_page(table->pager, right_child_page_num);
  // Increment only after the full check above, so a full node never gets a
  // key slot without a key in it.
  *internal_node_num_keys(parent) = original_num_keys + 1;

  if (child_max_key > get_node_max_key(table->pager, right_child)) {
    // New child becomes right_child; old right_child moves into last cell.
    *internal_node_child(parent, original_num_keys) = right_child_page_num;
    *internal_node_key(parent, original_num_keys) =
        get_node_max_key(table->pager, right_child);
    *internal_node_right_child(parent) = child_page_num;
  } else {
    // Shift cells right to make room for the new cell at index.
    for (uint32_t i = original_num_keys; i > index; i--) {
      void *destination = internal_node_cell(parent, i);
      void *source = internal_node_cell(parent, i - 1);
      memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_child(parent, index) = child_page_num;
    *internal_node_key(parent, index) = child_max_key;
  }
}

// The child whose max key was old_key now has max key new_key.
void update_internal_node_key(void *node, uint32_t old_key, uint32_t new_key) {
  uint32_t old_child_index = internal_node_find_child(node, old_key);
  // The right child has no key stored in this node, so there is nothing to
  // update; its max is tracked by this node's own parent.
  if (old_child_index < *internal_node_num_keys(node)) {
    *internal_node_key(node, old_child_index) = new_key;
  }
}

/*
 * internal_node_split_and_insert
 *
 * INTERNAL_NODE_MAX_CELLS = 3, so a full node has 4 children. Adding one
 * more means 5 children, split between two nodes.
 *
 * Example: child X (max key 15) must go into this full node:
 *
 *     node = [ c0 | 5 | c1 | 10 | c2 | 12 | right = c3 ]    (c3 max = 20)
 *
 * Step 1: list every child in key order, X included:
 *
 *     children:  c0(5)  c1(10)  c2(12)  X(15)  c3(20)
 *
 * Step 2: cut the list. The left node keeps the first MAX/2 + 1 = 2 old
 *         children, plus X if X sorts among them; the rest go right:
 *
 *     left:  c0 c1          right: c2 X c3
 *
 * Step 3: write each half as an internal node (its last child becomes
 *         right_child) and point the children's parent at their new node:
 *
 *     left  = [ c0 | 5 | right = c1 ]
 *     right = [ c2 | 12 | X | 15 | right = c3 ]
 *
 * Step 4: hook both halves into the tree.
 *   - Not the root: left reuses this node's page. In the parent, this node's
 *     key drops from 20 to 10, then right is inserted. That insert may split
 *     the parent too, and so on up to the root.
 *   - The root: it must stay on page 0, so both halves go to new pages and
 *     page 0 becomes [ left | 10 | right = right ]. The tree grows a level.
 */
void internal_node_split_and_insert(Table *table, uint32_t node_page_num,
                                    uint32_t child_page_num) {
  Pager *pager = table->pager;
  void *node = get_page(pager, node_page_num);
  uint32_t old_max = get_node_max_key(pager, node);
  uint32_t child_max = get_node_max_key(pager, get_page(pager, child_page_num));

  // Step 1: the node's children in key order, then X slotted in.
  uint32_t children[INTERNAL_NODE_MAX_CELLS + 2];
  uint32_t count = *internal_node_num_keys(node) + 1;
  for (uint32_t i = 0; i < count; i++) {
    children[i] = *internal_node_child(node, i);
  }
  uint32_t x = 0;
  while (x < count &&
         get_node_max_key(pager, get_page(pager, children[x])) < child_max) {
    x++;
  }
  memmove(&children[x + 1], &children[x], (count - x) * sizeof(uint32_t));
  children[x] = child_page_num;
  count++;

  // Step 2: where to cut.
  uint32_t left_count = INTERNAL_NODE_MAX_CELLS / 2 + 1;
  if (x < left_count) {
    left_count++;
  }

  // Step 3: write both halves. Each page is fetched right after it is
  // picked, so the next get_unused_page_num returns a different page.
  bool splitting_root = is_node_root(node);
  uint32_t parent_page_num = *node_parent(node);
  uint32_t left_page_num = node_page_num;
  if (splitting_root) {
    left_page_num = get_unused_page_num(pager);
  }
  void *left = get_page(pager, left_page_num);
  uint32_t right_page_num = get_unused_page_num(pager);
  void *right = get_page(pager, right_page_num);

  init_internal_node(left);
  init_internal_node(right);
  internal_node_set_children(pager, left_page_num, children, left_count);
  internal_node_set_children(pager, right_page_num, children + left_count,
                             count - left_count);

  // Step 4: hook both halves into the tree.
  if (splitting_root) {
    set_root_children(table, left_page_num, right_page_num);
    return;
  }
  void *parent = get_page(pager, parent_page_num);
  update_internal_node_key(parent, old_max, get_node_max_key(pager, left));
  *node_parent(right) = parent_page_num;
  internal_node_insert(table, parent_page_num, right_page_num);
}

/*
 * leaf_node_split_and_insert
 *
 * LEAF_NODE_MAX_CELLS = 13, so a full leaf + 1 new row = 14 rows,
 * split 7 (LEFT_SPLIT_COUNT) / 7 (RIGHT_SPLIT_COUNT).
 *
 * Example: old_node is full, insert key 5 -> cursor->cell_num = 4
 *
 *   old_node (before):
 *     idx:  0  1  2  3  4  5  6  7  8  9 10 11 12
 *     key:  1  2  3  4  6  7  8  9 10 11 12 13 14
 *
 * Step 1: think of a virtual 14-slot array (i = 0..13):
 *
 *     i:    0  1  2  3  4  5  6  7  8  9 10 11 12 13
 *     key:  1  2  3  4 [5] 6  7  8  9 10 11 12 13 14
 *                       ^
 *     i <  cell_num  -> old[i]       (unchanged)
 *     i == cell_num  -> new row
 *     i >  cell_num  -> old[i - 1]   (shifted right by one)
 *
 * Step 2: route each slot i to a node:
 *
 *     i <  7 -> old_node[i]          (left)
 *     i >= 7 -> new_node[i - 7]      (right)
 *
 *     i:    0  1  2  3  4  5  6 | 7  8  9 10 11 12 13
 *           <----- old_node ---->|<------ new_node ------>
 *
 *     old_node (page 0)                new_node (page 1)
 *     +---------------------+          +----------------------+
 *     | 1  2  3  4  5  6  7 |          | 8  9 10 11 12 13 14  |
 *     +---------------------+          +----------------------+
 *       idx 0..6                         idx 0..6  (i = 7..13)
 *
 * Why loop from i = 13 down to 0?
 *   old_node is overwritten in place. Slot i only reads old[i] or
 *   old[i-1], both <= i, which haven't been overwritten yet when we
 *   walk right-to-left:
 *
 *     i=6: old[6] <- old[5]    (read 5, write 6)
 *     i=5: old[5] <- old[4]    (read 4, write 5)
 *     i=4: old[4] <- NEW ROW
 *     i=3: old[3] <- old[3]    (no-op)
 *     ...
 *
 * Step 0 (before moving cells): splice new_node into the leaf chain
 * right after old_node:
 *
 *     before:  old_node --next--> X
 *     after:   old_node --next--> new_node --next--> X
 *
 *     new_node.next_leaf = old_node.next_leaf   (X, or 0 if none)
 *     old_node.next_leaf = new_page_num
 *
 * Step 3: set num_cells: old = 7, new = 7
 *
 * Step 4: old_node is root -> create_new_root(table, new_page_num)
 *         (see diagram above create_new_root)
 */
void leaf_node_split_and_insert(Cursor *cursor, uint32_t key, Row *value) {
  // Create a new node and move half the cell over.
  // Insert the new value in one of the two nodes.
  // Update parent or create new parent.

  void *old_node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t old_max = get_node_max_key(cursor->table->pager, old_node);
  uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
  void *new_node = get_page(cursor->table->pager, new_page_num);
  init_leaf_node(new_node);
  *node_parent(new_node) = *node_parent(old_node);
  *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
  *leaf_node_next_leaf(old_node) = new_page_num;

  // All existing keys plus new key should be divided
  // evenly between old (left) and new (right) nodes.
  // starting from the right, move each key to correct position.

  for (uint32_t n = LEAF_NODE_MAX_CELLS + 1; n > 0; n--) {
    uint32_t i = n - 1; // slot in the virtual array, 13 down to 0
    void *destination_node;
    if (i >= LEAF_NODE_LEFT_SPLIT_COUNT) {
      destination_node = new_node;
    } else {
      destination_node = old_node;
    }
    uint32_t index_within_node =
        i >= LEAF_NODE_LEFT_SPLIT_COUNT ? i - LEAF_NODE_LEFT_SPLIT_COUNT : i;
    void *destination = leaf_node_cell(destination_node, index_within_node);

    if (i == cursor->cell_num) {
      serialize_row(value,
                    leaf_node_value(destination_node, index_within_node));
      *leaf_node_key(destination_node, index_within_node) = key;
    } else if (i > cursor->cell_num) {
      memcpy(destination, leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
    } else {
      memcpy(destination, leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
    }
  }
  // update cell count on both leaf nodes.
  *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
  *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

  if (is_node_root(old_node)) {
    create_new_root(cursor->table, new_page_num);
  } else {
    uint32_t parent_page_num = *node_parent(old_node);
    uint32_t new_max = get_node_max_key(cursor->table->pager, old_node);
    void *parent = get_page(cursor->table->pager, parent_page_num);

    update_internal_node_key(parent, old_max, new_max);
    internal_node_insert(cursor->table, parent_page_num, new_page_num);
  }
}

void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value) {
  void *node = get_page(cursor->table->pager, cursor->page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= LEAF_NODE_MAX_CELLS) {
    leaf_node_split_and_insert(cursor, key, value);
    return;
  }

  if (cursor->cell_num < num_cells) {
    for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
      memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1),
             LEAF_NODE_CELL_SIZE);
    }
  }
  *(leaf_node_num_cells(node)) += 1;
  *(leaf_node_key(node, cursor->cell_num)) = key;
  serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

// ============================================================================
// Cursor: walk the rows in key order
// ============================================================================

Cursor *table_start(Table *table) {
  Cursor *cursor = table_find(table, 0);

  void *node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  cursor->end_of_table = (num_cells == 0);

  return cursor;
}

void cursor_advance(Cursor *cursor) {
  uint32_t page_num = cursor->page_num;
  void *node = get_page(cursor->table->pager, page_num);
  cursor->cell_num += 1;
  if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
    // advance to next leaf node.
    uint32_t next_page_num = *leaf_node_next_leaf(node);
    if (next_page_num == NO_NEXT_LEAF) {
      // rightmost leaf.
      cursor->end_of_table = true;
    } else {
      cursor->page_num = next_page_num;
      cursor->cell_num = 0;
    }
  }
}

void *cursor_value(Cursor *cursor) {
  uint32_t page_num = cursor->page_num;

  void *page = get_page(cursor->table->pager, page_num);
  return leaf_node_value(page, cursor->cell_num);
}

// ============================================================================
// Opening and closing the database
// ============================================================================

Table *db_open(const char *filename) {
  Pager *pager = pager_open(filename);

  Table *table = (Table *)malloc(sizeof(Table));
  table->pager = pager;
  table->root_page_num = 0;

  if (pager->num_pages == 0) {
    void *root_node = get_page(pager, 0);
    init_leaf_node(root_node);
    set_node_root(root_node, true);
  }

  return table;
}

void db_close(Table *table) {
  //  flushes the page cache to disk
  //  closes the database file
  //  frees the memory for the Pager and Table data structures
  Pager *pager = table->pager;
  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (pager->pages[i] == NULL) {
      continue;
    }

    pager_flush(pager, i);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  int result = close(pager->file_descriptor);
  if (result == -1) {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    void *page = pager->pages[i];
    if (page) {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}

// ============================================================================
// Debug output: .constants and .btree
// ============================================================================

void indent(uint32_t level) {
  for (uint32_t i = 0; i < level; i++) {
    printf("  ");
  }
}

void print_constants() {
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

// ============================================================================
// REPL: read, parse, execute
// ============================================================================

typedef struct {
  char *buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

InputBuffer *new_input_buffer() {
  InputBuffer *input_buffer = (InputBuffer *)malloc(sizeof(InputBuffer));
  input_buffer->buffer = NULL;
  input_buffer->buffer_length = 0;
  input_buffer->input_length = 0;
  return input_buffer;
}

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
  PREPARE_SUCCESS,
  PREPARE_UNRECOGNIZED_STATEMENT,
  PREPARE_STRING_TOO_LONG,
  PREPARE_NEGATIVE_ID,
  PREPARE_SYNTAX_ERROR,
} PrepareResult;

typedef enum { EXECUTE_SUCCESS, EXECUTE_DUPLICATE_KEY } ExecuteResult;

typedef enum { STATEMENT_INSERT, STATEMENT_SELECT } StatementType;

typedef struct {
  StatementType type;
  Row row_to_insert;
} Statement;

void print_prompt() { printf("db > "); }

ssize_t getline(char **lineptr, size_t *n, FILE *stream);

void read_input(InputBuffer *input_buffer) {
  ssize_t bytes_read =
      getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
  if (bytes_read <= 0) {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }

  input_buffer->input_length = bytes_read - 1;
  input_buffer->buffer[bytes_read - 1] = 0;
}

void close_input_buffer(InputBuffer *input_buffer) {
  free(input_buffer->buffer);
  free(input_buffer);
}

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table) {
  if (strcmp(input_buffer->buffer, ".exit") == 0) {
    db_close(table);
    exit(EXIT_SUCCESS);
  } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
    printf("Constants:\n");
    print_constants();
    return META_COMMAND_SUCCESS;
  } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
    printf("Tree:\n");
    print_tree(table->pager, 0, 0);
    return META_COMMAND_SUCCESS;
  } else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement) {

  statement->type = STATEMENT_INSERT;

  strtok(input_buffer->buffer, " "); // skip the "insert" keyword
  char *id_string = strtok(NULL, " ");
  char *username = strtok(NULL, " ");
  char *email = strtok(NULL, " ");

  if (id_string == NULL || username == NULL || email == NULL) {
    return PREPARE_SYNTAX_ERROR;
  }

  int id = atoi(id_string);
  if (id < 0) {
    return PREPARE_NEGATIVE_ID;
  }
  if (strlen(username) > COLUMN_USERNAME_SIZE) {
    return PREPARE_STRING_TOO_LONG;
  }

  if (strlen(email) > COLUMN_EMAIL_SIZE) {
    return PREPARE_STRING_TOO_LONG;
  }
  statement->row_to_insert.id = id;
  strcpy(statement->row_to_insert.username, username);
  strcpy(statement->row_to_insert.email, email);

  return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer *input_buffer,
                                Statement *statement) {
  if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
    return prepare_insert(input_buffer, statement);
  }

  if (strncmp(input_buffer->buffer, "select", 6) == 0) {
    statement->type = STATEMENT_SELECT;
    return PREPARE_SUCCESS;
  }

  return PREPARE_UNRECOGNIZED_STATEMENT;
}

ExecuteResult execute_insert(Statement *statement, Table *table) {
  Row *row_to_insert = &(statement->row_to_insert);
  uint32_t key_to_insert = row_to_insert->id;
  Cursor *cursor = table_find(table, key_to_insert);

  // Check the leaf the cursor landed on, not the root: once the root has
  // split it is an internal node and holds no rows.
  void *node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  if (cursor->cell_num < num_cells) {
    uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
    if (key_at_index == key_to_insert) {
      free(cursor);
      return EXECUTE_DUPLICATE_KEY;
    }
  }
  leaf_node_insert(cursor, row_to_insert->id, row_to_insert);

  free(cursor);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement *statement, Table *table) {
  Cursor *cursor = table_start(table);
  Row row;
  while (!(cursor->end_of_table)) {
    deserialize_row(cursor_value(cursor), &row);
    print_row(&row);
    cursor_advance(cursor);
  }
  free(cursor);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement *statement, Table *table) {
  switch (statement->type) {
  case (STATEMENT_INSERT):
    return execute_insert(statement, table);
  case (STATEMENT_SELECT):
    return execute_select(statement, table);
  }
  printf("Unknown statement type %d\n", statement->type);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Must supply a database filename.\n");
    exit(EXIT_FAILURE);
  }
  Table *table = db_open(argv[1]);
  InputBuffer *input_buffer = new_input_buffer();
  while (1) {
    print_prompt();
    read_input(input_buffer);

    if (input_buffer->buffer[0] == '.') {
      switch (do_meta_command(input_buffer, table)) {
      case (META_COMMAND_SUCCESS):
        continue;
      case (META_COMMAND_UNRECOGNIZED_COMMAND):
        printf("Unrecognized command '%s'\n", input_buffer->buffer);
        continue;
      }
    }
    Statement statement;

    switch (prepare_statement(input_buffer, &statement)) {
    case (PREPARE_SUCCESS):
      break;
    case (PREPARE_UNRECOGNIZED_STATEMENT):
      printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
      continue;

    case (PREPARE_STRING_TOO_LONG):
      printf("String is too long.\n");
      continue;
    case (PREPARE_NEGATIVE_ID):
      printf("ID must be positive.\n");
      continue;

    case (PREPARE_SYNTAX_ERROR):
      printf("Syntax error. Could not parse statement.\n");
      continue;
    }
    switch (execute_statement(&statement, table)) {
    case (EXECUTE_SUCCESS):
      printf("Executed.\n");
      break;
    case (EXECUTE_DUPLICATE_KEY):
      printf("Error: Duplicate key.\n");
      break;
    }
  }
}
