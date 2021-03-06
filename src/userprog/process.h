#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stdbool.h>
#include "threads/thread.h"
#include "userprog/syscall.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void mapping_destroy (struct hash_elem *m_, void *aux UNUSED);

#define MAXARGS 100

#endif /* userprog/process.h */
