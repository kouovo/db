// Black-box tests: feed commands to ./db on stdin and compare its stdout.
// Build and run:  gcc test.c -o test_db && ./test_db   (build ./db first)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB "./db"
#define DB_FILE "test.db"

static int pass = 0;
static int fail = 0;

// Runs ./db with `input` on stdin and returns everything it wrote to stdout.
// Leaves DB_FILE in place, so consecutive calls share one database.
// The caller frees the result.
static char *run_db_keep(const char *input) {
  // Input goes through a temp file rather than a pipe, so a large script
  // cannot deadlock against the child's output.
  FILE *in = tmpfile();
  int out[2];
  if (in == NULL || pipe(out) != 0) {
    perror("run_db");
    exit(EXIT_FAILURE);
  }
  fputs(input, in);
  rewind(in);

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(EXIT_FAILURE);
  }
  if (pid == 0) {
    dup2(fileno(in), STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);
    close(out[0]);
    close(out[1]);
    execl(DB, DB, DB_FILE, (char *)NULL);
    perror("exec " DB);
    _exit(127);
  }
  close(out[1]);
  fclose(in);

  size_t length = 0, capacity = 4096;
  char *output = malloc(capacity);
  ssize_t bytes_read;
  while ((bytes_read = read(out[0], output + length, capacity - length - 1)) >
         0) {
    length += bytes_read;
    if (capacity - length < 2) {
      capacity *= 2;
      output = realloc(output, capacity);
    }
  }
  output[length] = 0;
  close(out[0]);

  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    fprintf(stderr, "Could not run %s. Build it first.\n", DB);
    exit(EXIT_FAILURE);
  }
  return output;
}

// Like run_db_keep, but starts from an empty database and removes it after.
static char *run_db(const char *input) {
  unlink(DB_FILE);
  char *output = run_db_keep(input);
  unlink(DB_FILE);
  return output;
}

static void report(const char *name, int ok, const char *expected,
                   const char *actual) {
  if (ok) {
    printf("PASS  %s\n", name);
    pass++;
  } else {
    printf("FAIL  %s\n--- expected ---\n%s\n--- actual ---\n%s\n---\n", name,
           expected, actual);
    fail++;
  }
}

// Appends one insert line per key to `s` and returns the new end of `s`.
static char *append_inserts(char *s, const int *keys, size_t count) {
  for (size_t i = 0; i < count; i++) {
    s += sprintf(s, "insert %d user%d person%d@example.com\n", keys[i],
                 keys[i], keys[i]);
  }
  return s;
}

// Appends the select output for keys first..last (ascending) to `s`.
static char *append_rows(char *s, int first, int last) {
  for (int k = first; k <= last; k++) {
    s += sprintf(s, "(%d, user%d, person%d@example.com)\n", k, k, k);
  }
  return s;
}

// The whole output must equal `expected`.
static void check(const char *name, const char *input, const char *expected) {
  char *actual = run_db(input);
  report(name, strcmp(actual, expected) == 0, expected, actual);
  free(actual);
}

// The output must end with `expected`.
static void check_tail(const char *name, const char *input,
                       const char *expected) {
  char *actual = run_db(input);
  size_t actual_length = strlen(actual), expected_length = strlen(expected);
  const char *tail = actual_length >= expected_length
                         ? actual + actual_length - expected_length
                         : actual;
  report(name, strcmp(tail, expected) == 0, expected, tail);
  free(actual);
}

int main(void) {
  check("inserts and retrieves a row",
        "insert 1 user1 person1@example.com\n"
        "select\n"
        ".exit\n",
        "db > Executed.\n"
        "db > (1, user1, person1@example.com)\n"
        "Executed.\n"
        "db > ");

  check("reports a syntax error",
        "insert 1 user1\n"
        ".exit\n",
        "db > Syntax error. Could not parse statement.\n"
        "db > ");

  check("rejects an unrecognized statement",
        "foo\n"
        ".exit\n",
        "db > Unrecognized keyword at start of 'foo'.\n"
        "db > ");

  check("rejects an unrecognized meta-command",
        ".foo\n"
        ".exit\n",
        "db > Unrecognized command '.foo'\n"
        "db > ");

  char username[33], email[256];
  memset(username, 'a', 32);
  username[32] = 0;
  memset(email, 'a', 255);
  email[255] = 0;

  char input[1024], expected[1024];

  snprintf(input, sizeof(input), "insert 1 %s %s\nselect\n.exit\n", username,
           email);
  snprintf(expected, sizeof(expected),
           "db > Executed.\ndb > (1, %s, %s)\nExecuted.\ndb > ", username,
           email);
  check("allows strings of maximum length", input, expected);

  snprintf(input, sizeof(input), "insert 1 %sa %sa\nselect\n.exit\n", username,
           email);
  check("rejects strings that are too long", input,
        "db > String is too long.\n"
        "db > Executed.\n"
        "db > ");

  check("rejects a negative id",
        "insert -1 cstack foo@bar.com\n"
        "select\n"
        ".exit\n",
        "db > ID must be positive.\n"
        "db > Executed.\n"
        "db > ");

  check("selects nothing from an empty table",
        "select\n"
        ".exit\n",
        "db > Executed.\n"
        "db > ");

  check("prints constants",
        ".constants\n"
        ".exit\n",
        "db > Constants:\n"
        "ROW_SIZE: 293\n"
        "COMMON_NODE_HEADER_SIZE: 6\n"
        "LEAF_NODE_HEADER_SIZE: 14\n"
        "LEAF_NODE_CELL_SIZE: 297\n"
        "LEAF_NODE_SPACE_FOR_CELLS: 4082\n"
        "LEAF_NODE_MAX_CELLS: 13\n"
        "db > ");

  check("prints an empty leaf node",
        ".btree\n"
        ".exit\n",
        "db > Tree:\n"
        "- leaf (size 0)\n"
        "db > ");

  check("prints the structure of a one-node btree",
        "insert 1 user1 person1@example.com\n"
        "insert 2 user2 person2@example.com\n"
        "insert 3 user3 person3@example.com\n"
        ".btree\n"
        ".exit\n",
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Tree:\n"
        "- leaf (size 3)\n"
        "  - 1\n"
        "  - 2\n"
        "  - 3\n"
        "db > ");

  check("keeps keys sorted when inserted out of order",
        "insert 3 user3 person3@example.com\n"
        "insert 1 user1 person1@example.com\n"
        "insert 2 user2 person2@example.com\n"
        ".btree\n"
        ".exit\n",
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Tree:\n"
        "- leaf (size 3)\n"
        "  - 1\n"
        "  - 2\n"
        "  - 3\n"
        "db > ");

  // Inserting before existing cells shifts them: each row must still travel
  // with its own key.
  check("selects rows in key order with their own data",
        "insert 5 user5 person5@example.com\n"
        "insert 1 user1 person1@example.com\n"
        "insert 3 user3 person3@example.com\n"
        "insert 4 user4 person4@example.com\n"
        "insert 2 user2 person2@example.com\n"
        "select\n"
        ".exit\n",
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > (1, user1, person1@example.com)\n"
        "(2, user2, person2@example.com)\n"
        "(3, user3, person3@example.com)\n"
        "(4, user4, person4@example.com)\n"
        "(5, user5, person5@example.com)\n"
        "Executed.\n"
        "db > ");

  check("rejects a duplicate key",
        "insert 1 user1 person1@example.com\n"
        "insert 1 user1 person1@example.com\n"
        "select\n"
        ".exit\n",
        "db > Executed.\n"
        "db > Error: Duplicate key.\n"
        "db > (1, user1, person1@example.com)\n"
        "Executed.\n"
        "db > ");

  // The duplicate may sit at the start, middle or end of the node, and the
  // rejected row must not replace the stored one.
  check("rejects a duplicate key at any position",
        "insert 1 user1 person1@example.com\n"
        "insert 2 user2 person2@example.com\n"
        "insert 3 user3 person3@example.com\n"
        "insert 1 other other@example.com\n"
        "insert 2 other other@example.com\n"
        "insert 3 other other@example.com\n"
        "select\n"
        ".exit\n",
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Executed.\n"
        "db > Error: Duplicate key.\n"
        "db > Error: Duplicate key.\n"
        "db > Error: Duplicate key.\n"
        "db > (1, user1, person1@example.com)\n"
        "(2, user2, person2@example.com)\n"
        "(3, user3, person3@example.com)\n"
        "Executed.\n"
        "db > ");

  // Persistence: the second run reopens the file the first one wrote.
  unlink(DB_FILE);
  free(run_db_keep("insert 1 user1 person1@example.com\n"
                   "insert 2 user2 person2@example.com\n"
                   ".exit\n"));
  char *reopened = run_db_keep("select\n"
                               ".btree\n"
                               ".exit\n");
  const char *persisted = "db > (1, user1, person1@example.com)\n"
                          "(2, user2, person2@example.com)\n"
                          "Executed.\n"
                          "db > Tree:\n"
                          "- leaf (size 2)\n"
                          "  - 1\n"
                          "  - 2\n"
                          "db > ";
  report("keeps data after closing the connection",
         strcmp(reopened, persisted) == 0, persisted, reopened);
  free(reopened);

  // A third run inserts into what is already on disk, and still sees the
  // stored keys as duplicates.
  char *appended = run_db_keep("insert 3 user3 person3@example.com\n"
                               "insert 2 other other@example.com\n"
                               "select\n"
                               ".exit\n");
  const char *appended_expected = "db > Executed.\n"
                                  "db > Error: Duplicate key.\n"
                                  "db > (1, user1, person1@example.com)\n"
                                  "(2, user2, person2@example.com)\n"
                                  "(3, user3, person3@example.com)\n"
                                  "Executed.\n"
                                  "db > ";
  report("inserts into a reopened database",
         strcmp(appended, appended_expected) == 0, appended_expected, appended);
  free(appended);
  unlink(DB_FILE);

  // A leaf node holds LEAF_NODE_MAX_CELLS (13) rows: 13 inserts succeed and
  // all come back from a single leaf.
  enum { MAX_CELLS = 13 };
  char *script = malloc((MAX_CELLS + 1) * 64 + 32);
  char *full_expected = malloc((MAX_CELLS + 1) * 64 + 64);
  char *cursor = script;
  char *expected_cursor = full_expected;
  for (int i = 1; i <= MAX_CELLS; i++) {
    cursor +=
        sprintf(cursor, "insert %d user%d person%d@example.com\n", i, i, i);
    expected_cursor += sprintf(expected_cursor, "db > Executed.\n");
  }
  strcpy(cursor, "select\n.exit\n");
  expected_cursor += sprintf(expected_cursor, "db > ");
  for (int i = 1; i <= MAX_CELLS; i++) {
    expected_cursor += sprintf(expected_cursor,
                               "(%d, user%d, person%d@example.com)\n", i, i, i);
  }
  strcpy(expected_cursor, "Executed.\ndb > ");
  check("fills a leaf node and reads every row back", script, full_expected);

  free(script);
  free(full_expected);

  // Multi-level tree tests. Buffers are sized for up to 32 inserts.
  char tree_script[4096], tree_expected[4096];
  char *ts, *te;

  // From the book (part 11): the 14th insert splits the root into two
  // leaves, and the 15th insert finds its leaf through the internal node.
  ts = tree_script;
  for (int i = 1; i <= 14; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ts, ".btree\n"
             "insert 15 user15 person15@example.com\n"
             ".exit\n");
  check_tail("allows printing out the structure of a 3-leaf-node btree",
             tree_script,
             "db > Tree:\n"
             "- internal (size 1)\n"
             "  - leaf (size 7)\n"
             "    - 1\n"
             "    - 2\n"
             "    - 3\n"
             "    - 4\n"
             "    - 5\n"
             "    - 6\n"
             "    - 7\n"
             "  - key 7\n"
             "  - leaf (size 7)\n"
             "    - 8\n"
             "    - 9\n"
             "    - 10\n"
             "    - 11\n"
             "    - 12\n"
             "    - 13\n"
             "    - 14\n"
             "db > Executed.\n"
             "db > ");

  // From the book (part 12): select follows next_leaf from the first leaf
  // into the second, and every row keeps its own data across the split.
  ts = tree_script;
  te = tree_expected;
  for (int i = 1; i <= 15; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ts, "select\n.exit\n");
  te += sprintf(te, "db > ");
  for (int i = 1; i <= 15; i++) {
    te += sprintf(te, "(%d, user%d, person%d@example.com)\n", i, i, i);
  }
  strcpy(te, "Executed.\ndb > ");
  check_tail("prints all rows in a multi-level tree", tree_script,
             tree_expected);

  // After a split the root is an internal node, so the duplicate check has
  // to look in the leaf the key belongs to. Try the first and last key of
  // each leaf, and make sure no stored row is replaced.
  ts = tree_script;
  for (int i = 1; i <= 14; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  int duplicates[] = {1, 7, 8, 14};
  te = tree_expected;
  for (size_t i = 0; i < sizeof(duplicates) / sizeof(duplicates[0]); i++) {
    ts += sprintf(ts, "insert %d other other@example.com\n", duplicates[i]);
    te += sprintf(te, "db > Error: Duplicate key.\n");
  }
  strcpy(ts, "select\n.exit\n");
  te += sprintf(te, "db > ");
  for (int i = 1; i <= 14; i++) {
    te += sprintf(te, "(%d, user%d, person%d@example.com)\n", i, i, i);
  }
  strcpy(te, "Executed.\ndb > ");
  check_tail("rejects a duplicate key after the root splits", tree_script,
             tree_expected);

  // From the book (part 13): splitting a non-root leaf now updates its
  // parent. The script inserts keys out of order, so new cells land in the
  // middle of the internal node, and the tree grows to four leaves.
  int out_of_order[] = {18, 7,  10, 29, 23, 4,  14, 30, 15, 26,
                        22, 19, 2,  1,  21, 11, 6,  20, 5,  8,
                        9,  3,  12, 27, 17, 16, 13, 24, 25, 28};
  ts = tree_script;
  for (size_t i = 0; i < sizeof(out_of_order) / sizeof(out_of_order[0]);
       i++) {
    int k = out_of_order[i];
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", k, k, k);
  }
  strcpy(ts, ".btree\n.exit\n");
  te = tree_expected;
  te += sprintf(te, "db > Tree:\n- internal (size 3)\n");
  int leaf_ends[] = {7, 15, 22, 30};
  int first = 1;
  for (int leaf = 0; leaf < 4; leaf++) {
    int last = leaf_ends[leaf];
    te += sprintf(te, "  - leaf (size %d)\n", last - first + 1);
    for (int k = first; k <= last; k++) {
      te += sprintf(te, "    - %d\n", k);
    }
    if (leaf < 3) {
      te += sprintf(te, "  - key %d\n", last);
    }
    first = last + 1;
  }
  strcpy(te, "db > ");
  check_tail("allows printing out the structure of a 4-leaf-node btree",
             tree_script, tree_expected);

  // ---- Part 14: splitting internal nodes ----
  // INTERNAL_NODE_MAX_CELLS is 3, so an internal node holds at most 4
  // children. Sequential inserts fill leaves 7 + 7 + 7 + 13; the 35th insert
  // splits the last leaf into a 5th child, which splits the root internal
  // node: children {1-7, 8-14} stay left, {15-21, 22-28, 29-35} go right.
  ts = tree_script;
  for (int i = 1; i <= 35; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ts, ".btree\n.exit\n");
  te = tree_expected;
  te += sprintf(te, "db > Tree:\n- internal (size 1)\n");
  for (int leaf = 0; leaf < 5; leaf++) {
    int lo = leaf * 7 + 1, hi = leaf * 7 + 7;
    if (leaf == 0) {
      te += sprintf(te, "  - internal (size 1)\n");
    } else if (leaf == 2) {
      te += sprintf(te, "  - key 14\n  - internal (size 2)\n");
    }
    te += sprintf(te, "    - leaf (size 7)\n");
    for (int k = lo; k <= hi; k++) {
      te += sprintf(te, "      - %d\n", k);
    }
    if (leaf != 1 && leaf != 4) {
      te += sprintf(te, "    - key %d\n", hi);
    }
  }
  strcpy(te, "db > ");
  check_tail("splits the root internal node", tree_script, tree_expected);

  // From the book (part 14): 64 out-of-order inserts split internal nodes and
  // build a two-level internal tree over seven leaves.
  int book_keys[] = {58, 56, 8,  54, 77, 7,  25, 71, 13, 22, 53, 51, 59,
                     32, 36, 79, 10, 33, 20, 4,  35, 76, 49, 24, 70, 48,
                     39, 15, 47, 30, 86, 31, 68, 37, 66, 63, 40, 78, 19,
                     46, 14, 81, 72, 6,  50, 85, 67, 2,  55, 69, 5,  65,
                     52, 1,  29, 9,  43, 75, 21, 82, 12, 18, 60, 44};
  enum { BOOK_KEYS = sizeof(book_keys) / sizeof(book_keys[0]) };
  ts = append_inserts(tree_script, book_keys, BOOK_KEYS);
  strcpy(ts, ".btree\n.exit\n");
  check_tail("allows printing out the structure of a 7-leaf-node btree",
             tree_script,
             "db > Tree:\n"
             "- internal (size 1)\n"
             "  - internal (size 2)\n"
             "    - leaf (size 7)\n"
             "      - 1\n      - 2\n      - 4\n      - 5\n"
             "      - 6\n      - 7\n      - 8\n"
             "    - key 8\n"
             "    - leaf (size 11)\n"
             "      - 9\n      - 10\n      - 12\n      - 13\n"
             "      - 14\n      - 15\n      - 18\n      - 19\n"
             "      - 20\n      - 21\n      - 22\n"
             "    - key 22\n"
             "    - leaf (size 8)\n"
             "      - 24\n      - 25\n      - 29\n      - 30\n"
             "      - 31\n      - 32\n      - 33\n      - 35\n"
             "  - key 35\n"
             "  - internal (size 3)\n"
             "    - leaf (size 12)\n"
             "      - 36\n      - 37\n      - 39\n      - 40\n"
             "      - 43\n      - 44\n      - 46\n      - 47\n"
             "      - 48\n      - 49\n      - 50\n      - 51\n"
             "    - key 51\n"
             "    - leaf (size 11)\n"
             "      - 52\n      - 53\n      - 54\n      - 55\n"
             "      - 56\n      - 58\n      - 59\n      - 60\n"
             "      - 63\n      - 65\n      - 66\n"
             "    - key 66\n"
             "    - leaf (size 7)\n"
             "      - 67\n      - 68\n      - 69\n      - 70\n"
             "      - 71\n      - 72\n      - 75\n"
             "    - key 75\n"
             "    - leaf (size 8)\n"
             "      - 76\n      - 77\n      - 78\n      - 79\n"
             "      - 81\n      - 82\n      - 85\n      - 86\n"
             "db > ");

  // select walks the next_leaf chain across both internal subtrees and
  // returns every row, in key order, with its own data.
  ts = append_inserts(tree_script, book_keys, BOOK_KEYS);
  strcpy(ts, "select\n.exit\n");
  int sorted_keys[BOOK_KEYS];
  memcpy(sorted_keys, book_keys, sizeof(book_keys));
  for (int i = 1; i < BOOK_KEYS; i++) { // insertion sort
    int k = sorted_keys[i], j = i - 1;
    for (; j >= 0 && sorted_keys[j] > k; j--) {
      sorted_keys[j + 1] = sorted_keys[j];
    }
    sorted_keys[j + 1] = k;
  }
  te = tree_expected;
  te += sprintf(te, "db > ");
  for (int i = 0; i < BOOK_KEYS; i++) {
    te = append_rows(te, sorted_keys[i], sorted_keys[i]);
  }
  strcpy(te, "Executed.\ndb > ");
  check_tail("selects every row after internal nodes split", tree_script,
             tree_expected);

  // Duplicate lookups must descend through two internal levels: try keys
  // at both ends of both subtrees and on either side of the root key (35).
  int deep_duplicates[] = {1, 8, 35, 36, 75, 86};
  enum { DEEP_DUPS = sizeof(deep_duplicates) / sizeof(deep_duplicates[0]) };
  ts = append_inserts(tree_script, book_keys, BOOK_KEYS);
  te = tree_expected;
  for (int i = 0; i < DEEP_DUPS; i++) {
    ts += sprintf(ts, "insert %d other other@example.com\n",
                  deep_duplicates[i]);
    te += sprintf(te, "db > Error: Duplicate key.\n");
  }
  strcpy(ts, ".exit\n");
  strcpy(te, "db > ");
  check_tail("rejects duplicate keys in a multi-level internal tree",
             tree_script, tree_expected);

  // Parent pointers live on disk. After reopening, further splits must still
  // find the right parents: insert 1..35 (root internal split), close, then
  // insert 36..70 in a second run, which splits internal nodes again.
  unlink(DB_FILE);
  ts = tree_script;
  for (int i = 1; i <= 35; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ts, ".exit\n");
  free(run_db_keep(tree_script));
  ts = tree_script;
  for (int i = 36; i <= 70; i++) {
    ts += sprintf(ts, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ts, "select\n.exit\n");
  te = tree_expected;
  te += sprintf(te, "db > ");
  te = append_rows(te, 1, 70);
  strcpy(te, "Executed.\ndb > ");
  char *reopened_tree = run_db_keep(tree_script);
  size_t rt_len = strlen(reopened_tree), te_len = strlen(tree_expected);
  const char *rt_tail = rt_len >= te_len
                            ? reopened_tree + rt_len - te_len
                            : reopened_tree;
  report("splits internal nodes in a reopened database",
         strcmp(rt_tail, tree_expected) == 0, tree_expected, rt_tail);
  free(reopened_tree);
  unlink(DB_FILE);

  // Descending inserts always land in the leftmost leaf, so internal splits
  // repeatedly insert at index 0. 200 rows make the tree at least 3 internal
  // levels deep.
  enum { DESC_ROWS = 200 };
  char *desc_script = malloc(DESC_ROWS * 64 + 32);
  char *desc_expected = malloc(DESC_ROWS * 64 + 32);
  char *ds = desc_script;
  for (int i = DESC_ROWS; i >= 1; i--) {
    ds += sprintf(ds, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(ds, "select\n.exit\n");
  ds = desc_expected;
  ds += sprintf(ds, "db > ");
  ds = append_rows(ds, 1, DESC_ROWS);
  strcpy(ds, "Executed.\ndb > ");
  check_tail("selects every row after descending inserts", desc_script,
             desc_expected);
  free(desc_script);
  free(desc_expected);

  // Internal nodes now split, so the next limit is the pager:
  // TABLE_MAX_PAGES (100) pages run out before 1400 sequential rows fit.
  enum { MANY_ROWS = 1400 };
  char *big_script = malloc(MANY_ROWS * 64 + 16);
  char *bs = big_script;
  for (int i = 1; i <= MANY_ROWS; i++) {
    bs += sprintf(bs, "insert %d user%d person%d@example.com\n", i, i, i);
  }
  strcpy(bs, ".exit\n");
  check_tail("prints error message when table is full", big_script,
             "db > Executed.\n"
             "db > Tried to fetch page number out of bounds. 100 >= 100\n");
  free(big_script);

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
