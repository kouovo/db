#ifndef DB_BTREE_H
#define DB_BTREE_H

#include <stdbool.h>
#include <stdint.h>

#include "pager.h"
#include "row.h"

// Table: a B-tree of rows stored in a pager, plus cursors over it.

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

// Search: return the position of the given key, or where it should be
// inserted if it is not present.
Cursor *table_find(Table *table, uint32_t key);

// Insert: add a row at the cursor, splitting full nodes.
void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value);

// Cursor: walk the rows in key order.
Cursor *table_start(Table *table);
void cursor_advance(Cursor *cursor);
void *cursor_value(Cursor *cursor);

// Opening and closing the database.
Table *db_open(const char *filename);
void db_close(Table *table);

#endif
