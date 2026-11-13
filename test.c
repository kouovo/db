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
// The caller frees the result.
static char *run_db(const char *input) {
  // Input goes through a temp file rather than a pipe, so a large script
  // cannot deadlock against the child's output.
  unlink(DB_FILE); // every run starts from an empty database
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
  unlink(DB_FILE);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    fprintf(stderr, "Could not run %s. Build it first.\n", DB);
    exit(EXIT_FAILURE);
  }
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

  // Table full: insert 1401 rows and look only at the end of the output.
  char *script = malloc(1401 * 64 + 16);
  char *cursor = script;
  for (int i = 1; i <= 1401; i++) {
    cursor += sprintf(cursor, "insert %d user%d person%d@example.com\n", i, i,
                      i);
  }
  strcpy(cursor, ".exit\n");
  check_tail("reports an error when the table is full", script,
             "db > Error: Table full.\n"
             "db > ");
  free(script);

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
