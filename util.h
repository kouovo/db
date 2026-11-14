#ifndef DB_UTIL_H
#define DB_UTIL_H

#include <stdint.h>

// Address of the field `offset` bytes into a page (or a serialized row).
// Arithmetic on void * is a GNU extension, so go through char *.
static inline void *node_at(void *node, uint32_t offset) {
  return (char *)node + offset;
}

#endif
