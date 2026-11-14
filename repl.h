#ifndef DB_REPL_H
#define DB_REPL_H

#include <stddef.h>
#include <sys/types.h>

#include "btree.h"
#include "row.h"

// REPL: read, parse, execute

typedef struct {
  char *buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

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

InputBuffer *new_input_buffer(void);
void print_prompt(void);
void read_input(InputBuffer *input_buffer);
void close_input_buffer(InputBuffer *input_buffer);

MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table);
PrepareResult prepare_statement(InputBuffer *input_buffer,
                                Statement *statement);
ExecuteResult execute_statement(Statement *statement, Table *table);

#endif
