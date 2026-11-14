#include "row.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

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
