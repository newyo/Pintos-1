#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/off_t.h"

/* Struct that holds information about a page in the supplementary page table.
   This gets inserted into a thread's struct hash supp_pt member */
struct sup_page
  {
    struct file* file;
    bool writable;
    off_t offset;
    size_t zero_bytes;

    struct hash_elem pt_elem;
  };

struct sup_page* create_zero_page (void);
struct sup_page* create_full_page (struct file*, off_t offset, bool writable);
struct sup_page* create_partial_page (struct file*, off_t offset,
                                      size_t zero_bytes, bool writable);
bool add_sup_page (struct sup_page *page);

#endif
