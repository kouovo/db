#include "btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "node.h"

// ============================================================================
// Search: find where a key lives
// ============================================================================

static Cursor *leaf_node_find(Table *table, uint32_t page_num, uint32_t key) {
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

static uint32_t internal_node_find_child(void *node, uint32_t key) {
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
static void internal_node_set_children(Pager *pager, uint32_t page_num,
                                       const uint32_t *children,
                                       uint32_t count) {
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
static void set_root_children(Table *table, uint32_t left_page_num,
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
static void create_new_root(Table *table, uint32_t right_child_page_num) {
  void *root = get_page(table->pager, table->root_page_num);
  uint32_t left_child_page_num = get_unused_page_num(table->pager);
  void *left_child = get_page(table->pager, left_child_page_num);
  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);
  set_root_children(table, left_child_page_num, right_child_page_num);
}

static void internal_node_split_and_insert(Table *table, uint32_t node_page_num,
                                           uint32_t child_page_num);

// Adds child_page_num to the internal node at parent_page_num, keyed by the
// child's max key. Splits the parent first if it is full.
static void internal_node_insert(Table *table, uint32_t parent_page_num,
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
static void update_internal_node_key(void *node, uint32_t old_key,
                                     uint32_t new_key) {
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
static void internal_node_split_and_insert(Table *table, uint32_t node_page_num,
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
static void leaf_node_split_and_insert(Cursor *cursor, uint32_t key,
                                       Row *value) {
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
