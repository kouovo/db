#ifndef DB_PAGER_H
#define DB_PAGER_H

#include <stdint.h>

// Pager: cache of 4 KB file pages

#define PAGE_SIZE ((uint32_t)4096)
#define TABLE_MAX_PAGES 100

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  void *pages[TABLE_MAX_PAGES];
  uint32_t num_pages;
} Pager;

void *get_page(Pager *pager, uint32_t page_num);
uint32_t get_unused_page_num(Pager *pager);
Pager *pager_open(const char *filename);
void pager_flush(Pager *pager, uint32_t page_num);

#endif
