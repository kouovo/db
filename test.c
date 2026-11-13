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
        "LEAF_NODE_HEADER_SIZE: 10\n"
        "LEAF_NODE_CELL_SIZE: 297\n"
        "LEAF_NODE_SPACE_FOR_CELLS: 4086\n"
        "LEAF_NODE_MAX_CELLS: 13\n"
        "db > ");

  check("prints an empty leaf node",
        ".btree\n"
        ".exit\n",
        "db > BTree:\n"
        "leaf (size 0)\n"
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
        "db > BTree:\n"
        "leaf (size 3)\n"
        "  - 0 : 1\n"
        "  - 1 : 2\n"
        "  - 2 : 3\n"
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
                          "db > BTree:\n"
                          "leaf (size 2)\n"
                          "  - 0 : 1\n"
                          "  - 1 : 2\n"
                          "db > ";
  report("keeps data after closing the connection",
         strcmp(reopened, persisted) == 0, persisted, reopened);
  free(reopened);

  // A third run appends to what is already on disk.
  char *appended = run_db_keep("insert 3 user3 person3@example.com\n"
                               "select\n"
                               ".exit\n");
  const char *appended_expected = "db > Executed.\n"
                                  "db > (1, user1, person1@example.com)\n"
                                  "(2, user2, person2@example.com)\n"
                                  "(3, user3, person3@example.com)\n"
                                  "Executed.\n"
                                  "db > ";
  report("appends to a reopened database",
         strcmp(appended, appended_expected) == 0, appended_expected,
         appended);
  free(appended);
  unlink(DB_FILE);

  // A leaf node holds LEAF_NODE_MAX_CELLS (13) rows and there is no splitting
  // yet: 13 inserts succeed and all come back, the 14th is refused.
  enum { MAX_CELLS = 13 };
  char *script = malloc((MAX_CELLS + 1) * 64 + 32);
  char *full_expected = malloc((MAX_CELLS + 1) * 64 + 64);
  char *cursor = script;
  char *expected_cursor = full_expected;
  for (int i = 1; i <= MAX_CELLS; i++) {
    cursor += sprintf(cursor, "insert %d user%d person%d@example.com\n", i, i,
                      i);
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

  sprintf(cursor, "insert %d user%d person%d@example.com\n.exit\n",
          MAX_CELLS + 1, MAX_CELLS + 1, MAX_CELLS + 1);
  check_tail("reports an error when the table is full", script,
             "db > Executed.\n"
             "db > Error: Table full.\n"
             "db > ");
  free(script);
  free(full_expected);

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
