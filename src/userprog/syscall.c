#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include <hash.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/mmap.h"
#include "userprog/exception.h"
#include <string.h>
#include "vm/swap.h"
#include <bitmap.h>

static void syscall_handler (struct intr_frame *f);
static void halt (void);
static pid_t exec (const char *file);
static int wait (pid_t pid);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int read (int fd_ptr, void *buffer, unsigned length,
                 void *stack_pointer);
static int write (int fd, const void *buffer, unsigned size);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static mapid_t mmap (int fd, void *addr);
static void munmap (mapid_t mapping, bool del_and_free);

static struct hash fd_hash;
static int next_fd = 2;

static struct lock filesys_lock;

void lock_filesystem (void)
{
  if (!lock_held_by_current_thread (&filesys_lock))
    lock_acquire (&filesys_lock);
}

void release_filesystem (void)
{
  if (lock_held_by_current_thread (&filesys_lock))
    lock_release (&filesys_lock);
}

struct fd_node
  {
    struct hash_elem hash_elem;
    unsigned fd;
    struct thread *thread;
    struct file *file;
  };

struct fd
  {
    int fd;
    struct list_elem elem;
  };

static unsigned
hash_func (const struct hash_elem *node_, void *aux UNUSED)
{
  struct fd_node *node = hash_entry (node_, struct fd_node, hash_elem);

  ASSERT (node != NULL);

  return node->fd;
}

static bool
less_func (const struct hash_elem *a_, const struct hash_elem *b_,
                void *aux UNUSED)
{
  struct fd_node *a = hash_entry (a_, struct fd_node, hash_elem);
  struct fd_node *b = hash_entry (b_, struct fd_node, hash_elem);

  ASSERT (a != NULL);
  ASSERT (b != NULL);

  return a->fd < b->fd;
}

static void
destructor_func (struct hash_elem *e_, void *aux UNUSED)
{
  struct fd_node *e = hash_entry (e_, struct fd_node, hash_elem);

  ASSERT (e != NULL);

  free (e);
}

/* Returns a file * for a given int fd. Terminates the process with an error
   code if the fd is not mapped, or is stdin/stdout. */
static struct file *
fd_to_file (int fd)
{
  struct fd_node node;
  node.fd = fd;

  struct hash_elem *e = hash_find (&fd_hash, &node.hash_elem);

  /* fd isn't mapped. Terminate.
     stdin/stdout failure cases are also caught here. */
  if (e == NULL)
    {
      if (lock_held_by_current_thread (&filesys_lock))
        release_filesystem ();
      exit (-1);
    }

  struct fd_node *entry = hash_entry (e, struct fd_node, hash_elem);

  /* fd doesn't belong to the current thread. Terminate. */
  if (entry->thread != thread_current ())
    {
      if (lock_held_by_current_thread (&filesys_lock))
        release_filesystem ();
      exit (-1);
    }

  return entry->file;
}

void
syscall_init (void)
{
  if (!hash_init (&fd_hash, hash_func, less_func, NULL))
    PANIC ("Failed to allocate memory for file descriptor map");

  lock_init (&filesys_lock);

  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Checks that a pointer points to a valid user memory address, and is therefore
   safe to be dereferenced. If it's not safe, we terminate the process.
   Returns true iff the ptr can safely be dereferenced. */
static bool
is_safe_user_ptr (const void *ptr)
{
  struct thread *t = thread_current();
  if (ptr == NULL || !is_user_vaddr (ptr) ||
      pagedir_get_page (t->pagedir, ptr) == NULL)
    {
      /* Destroy thread. */
      exit (-1);
    	return false;
    }
  else
    {
      return true;
    }
}

/* Switch on the system call numbers defined in lib/syscall-nr.h, and call the
   appropriate system call. If the system call returns something, then put
   that value in f->eax. */
static void
syscall_handler (struct intr_frame *f)
{
  ASSERT (f != NULL);
  ASSERT (f->esp != NULL);

  uint32_t *stack_pointer = f->esp;

  if (is_safe_user_ptr (stack_pointer))
    {
      int syscall_number = *stack_pointer;

      switch (syscall_number)
        {
          case SYS_HALT:
            halt ();
            break;

          case SYS_EXIT:
            if (is_safe_user_ptr (stack_pointer + 1))
              exit (*(stack_pointer + 1));
            break;

          case SYS_EXEC:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = exec ((char *) *(stack_pointer + 1));
            break;

          case SYS_WAIT:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = wait (*(stack_pointer + 1));
            break;

          case SYS_CREATE:
            if (is_safe_user_ptr (stack_pointer + 1) &&
                is_safe_user_ptr (stack_pointer + 2))
              f->eax = create ((char *) *(stack_pointer + 1),
                               *(stack_pointer + 2));
            break;

          case SYS_REMOVE:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = remove ((char *) *(stack_pointer + 1));
            break;

          case SYS_OPEN:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = open ((char *) *(stack_pointer + 1));
            break;

          case SYS_FILESIZE:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = filesize (*(stack_pointer + 1));
            break;

          case SYS_READ:
            if (is_safe_user_ptr (stack_pointer + 1) &&
                is_safe_user_ptr (stack_pointer + 2) &&
                is_safe_user_ptr (stack_pointer + 3))
                  {
                    f->eax = read (*(stack_pointer + 1),
                                   (void *) *(stack_pointer + 2),
                                   *(stack_pointer + 3),
                                   stack_pointer);
                    break;
                  }
          case SYS_WRITE:
            if (is_safe_user_ptr (stack_pointer + 1) &&
                is_safe_user_ptr (stack_pointer + 2) &&
                is_safe_user_ptr (stack_pointer + 3))
              f->eax = write (*(stack_pointer + 1),
                              (void *) *(stack_pointer + 2),
                              *(stack_pointer + 3));
            break;

          case SYS_SEEK:
            if (is_safe_user_ptr (stack_pointer + 1) &&
                is_safe_user_ptr (stack_pointer + 2))
              seek (*(stack_pointer + 1), *(stack_pointer + 2));
            break;

          case SYS_TELL:
            if (is_safe_user_ptr (stack_pointer + 1))
              f->eax = tell (*(stack_pointer + 1));
            break;

          case SYS_CLOSE:
            if (is_safe_user_ptr (stack_pointer + 1))
              close (*(stack_pointer + 1));
            break;

          case SYS_MMAP:
            if (is_safe_user_ptr (stack_pointer + 1) &&
                is_safe_user_ptr (stack_pointer + 2))
              f->eax = mmap (*(stack_pointer + 1),
                             (void *) *(stack_pointer + 2));
            break;

          case SYS_MUNMAP:
            if (is_safe_user_ptr (stack_pointer + 1))
              munmap (*(stack_pointer + 1), true);
            break;

          default:
            printf("%i\n", syscall_number);
            NOT_REACHED ();
        }
    }
}

/* Terminates Pintos. */
static void
halt (void)
{
  shutdown_power_off ();
}

void
syscall_done (void)
{
  hash_destroy (&fd_hash, destructor_func);
}

/* Terminates the current user program, sending its exit status to the kernel.
   If the process's parent waits for it, this is the status that will be
   returned. */
void
exit (int status)
{
  struct thread *exiting_thread = thread_current();

  /* Print the terminating message */
  printf("%s: exit(%d)\n", exiting_thread->name, status);

  /* Set some information about the child, for process_wait */
  struct thread *parent = exiting_thread->parent;
  struct child_info *child = NULL;

  struct list_elem *elem = list_tail (&parent->children);
  while ((elem = list_prev (elem)) != list_head (&parent->children))
    {
      child = list_entry(elem, struct child_info, infoelem);
      if (child->id == exiting_thread->tid)
        break;
    }

  ASSERT (child != NULL);

  lock_acquire (&parent->cond_lock);
  child->has_exited = true;
  child->return_status = status;
  lock_release (&parent->cond_lock);

  struct list_elem *e, *next;
  for (e = list_begin (&exiting_thread->open_fds);
       e != list_end (&exiting_thread->open_fds);
       e = next)
    {
      struct fd *fd = list_entry (e, struct fd, elem);
      next = list_next (e); /* Need to remember where we're going next, since
                               close will remove itself from the list. */
      close (fd->fd);
    }

  struct hash_iterator i;
  hash_first (&i, &exiting_thread->file_map);
  while (hash_next (&i))
    {
      struct hash_elem *e = hash_cur (&i);
      struct mapping *m = hash_entry (e, struct mapping, elem);

      ASSERT (m != NULL);

      munmap (m->mapid, false);
    }
  hash_clear (&exiting_thread->file_map, mapping_destroy);

  thread_exit();
}

/* Runs the executable whose name is given in cmd_line, passing any given
   arguments, and returns the new process's pid, or -1 if loading the new
   process failed */
static pid_t
exec (const char *cmd_line)
{
  if (is_safe_user_ptr (cmd_line))
    {
      struct thread *current = thread_current ();

      current->child_status = LOADING;
      tid_t child_tid = process_execute (cmd_line);

      lock_acquire (&current->cond_lock);
      while (current->child_status == LOADING)
        cond_wait (&current->child_waiter, &current->cond_lock);
      lock_release (&current->cond_lock);

      return (current->child_status == FAILED) ? -1 : child_tid;
    }

  NOT_REACHED ();
}

/* Waits for a child process pid and retrieves the child's exit status. */
static int
wait (pid_t pid)
{
  return process_wait (pid);
}

/* Creates a new file called file initially initial_size bytes in size.
   Returns true iff successful. */
static bool
create (const char *file, unsigned initial_size)
{
  if (is_safe_user_ptr (file))
    {
      lock_filesystem ();
      bool status = filesys_create (file, initial_size);
      release_filesystem ();
      return status;
    }

  NOT_REACHED ();
}

/* Deletes the file called file. Returns true iff successful. */
static bool
remove (const char *file)
{
  if (is_safe_user_ptr (file))
    {
      lock_filesystem ();
      bool status = filesys_remove (file);
      release_filesystem ();
      return status;
    }

  NOT_REACHED ();
}

/* Opens the file called filename. Returns a non-negative integer handle, or
   -1 if the file could not be opened. */
static int
open (const char *filename)
{
  if (is_safe_user_ptr (filename))
    {
      lock_filesystem ();

      struct file *open_file = filesys_open (filename);
      if (open_file == NULL)
        {
          release_filesystem ();
          return -1;
        }

      /* Allocate an fd. */
      struct fd_node *node = malloc (sizeof (struct fd_node));
      if (node == NULL)
        PANIC ("Failed to allocate memory for file descriptor node");

      node->fd = next_fd++;
      node->thread = thread_current ();
      node->file = open_file;
      hash_insert (&fd_hash, &node->hash_elem);

      struct fd *fd = malloc (sizeof (struct fd));
      if (fd == NULL)
        PANIC ("Failed to allocate memory for file descriptor list node");
      fd->fd = node->fd;

      list_push_back (&thread_current ()->open_fds, &fd->elem);

      release_filesystem ();
      return node->fd;
    }

	NOT_REACHED ();
}

/* Returns the size, in bytes, of the file open as fd. */
static int
filesize (int fd)
{
  lock_filesystem ();
  int length = file_length (fd_to_file (fd));
  release_filesystem ();
  return length;
}

/* Reads size bytes from the file open as fd into buffer. Returns the number
   of bytes actually read, or -1 if the file could not be read.
   fd == 0 reads from the keyboard using input_getc(). */
static int
read (int fd, void *buffer, unsigned length, void *stack_pointer)
{
  if (fd == STDOUT_FILENO)
    {
      exit (-1);
    }
  else if (is_user_vaddr (buffer))
    {
      void *rd_buffer = pg_round_down (buffer);

      struct thread *cur = thread_current ();
      struct sup_page *page = get_sup_page (&cur->supp_pt, rd_buffer);

      /* Checking if we have to expand the stack. */
      if (page == NULL && stack_pointer - 32 <= buffer)
        {
          /* Run out of stack space. */
          if (PHYS_BASE - buffer > MAXSIZE)
            exit (-1);

          /* Check if we need to expand the stack for the first page that the
             buffer starts of */
          if (pagedir_get_page (cur->pagedir, rd_buffer) == NULL)
            {
              void *new_frame = allocate_frame (PAL_USER | PAL_ZERO);
              pagedir_set_page (cur->pagedir, rd_buffer, new_frame, true);
            }

          /* Loop up the stack, checking if we need to grow the stack at that
             point, until we reach PHYS_BASE or the current page is already
             loaded */
          unsigned counter;
          for (counter = PGSIZE;
               is_user_vaddr (rd_buffer + counter) &&
               pagedir_get_page (cur->pagedir, rd_buffer + counter) == NULL;
               counter += PGSIZE)
            {
              void *new_frame = allocate_frame (PAL_USER | PAL_ZERO);
              pagedir_set_page (cur->pagedir, rd_buffer + counter,
                                new_frame, true);
            }
        }

      if (fd == STDIN_FILENO)
        {
          unsigned i = 0;
          uint8_t *b = buffer;
          for (; i < length; ++i)
            b[i] = (char) input_getc ();

          return length;
        }

      /* If the buffer is safe to write to, either it has been loaded
         or can be loaded in a page fault. */
      else
        {
          lock_filesystem ();
          int size = file_read (fd_to_file (fd), buffer, length);
          release_filesystem ();
          return size;
        }
    }
  else
    {
      exit (-1);
    }
}

/* Writes size bytes from buffer to the open file fd. Returns the number of
   bytes actually written, which may be less than size.
   fd == 1 writes to the console. */
static int
write (int fd, const void *buffer, unsigned size)
{
  /* Can't write to standard input. */
  if (fd == STDIN_FILENO)
    exit (-1);
  else if (is_safe_user_ptr (buffer) && is_safe_user_ptr (buffer + size))
    {
      if (fd == STDOUT_FILENO)
        {
          if (size < MAX_PUTBUF)
            {
              putbuf (buffer, size);
              return size;
            }
          else
            {
              int offset = 0;
              while (size > MAX_PUTBUF)
                {
                  putbuf (buffer + offset, MAX_PUTBUF);
                  offset += MAX_PUTBUF;
                  size -= MAX_PUTBUF;
                }

              putbuf (buffer + offset, size);
              offset += size;
              return offset;
            }
        }
      else
        {
          lock_filesystem ();
          int length = file_write (fd_to_file (fd), buffer, size);
          release_filesystem ();
          return length;
        }
    }

  NOT_REACHED ();
}

/* Changes the next byte to be read/written in open file fd to position,
   expressed in bytes from the beginning of the file. */
static void
seek (int fd, unsigned position)
{
  lock_filesystem ();
  file_seek (fd_to_file (fd), position);
  release_filesystem ();
}

/* Returns the position of the next byte to be read or written in open file
   fd, expressed in bytes from the beginning of the file. */
static unsigned
tell (int fd)
{
  lock_filesystem ();
  unsigned next = file_tell (fd_to_file (fd));
  release_filesystem ();
  return next;
}

/* Closes file descriptor fd. Exiting or terminating a process implicitly
   closes all its open file descriptors, as if by calling this function for
   each one. */
static void
close (int fd)
{
  lock_filesystem ();

  /* Close the file. */
  file_close (fd_to_file (fd));

  /* Remove the fd from the map so it can't be closed twice. */
  struct fd_node node;
  node.fd = fd;
  hash_find (&fd_hash, &node.hash_elem);
  struct hash_elem *e = hash_delete (&fd_hash, &node.hash_elem);
  destructor_func (e, NULL);

  /* Remove from thread's open_fds for the same reason. */
  struct fd *f = NULL;
  struct list_elem *el;
  for (el = list_begin (&thread_current ()->open_fds);
       el != list_end (&thread_current ()->open_fds);
       el = list_next (el))
    {
      f = list_entry (el, struct fd, elem);
      if (f->fd == fd)
          break;
    }

  /* If fd_to_file hasn't exited our thread, but the fd isn't in our open_fds,
     then we have a problem. */
  ASSERT (f != NULL);

  list_remove (el);
  free (f);
  release_filesystem ();
}

/* Returns true iff a given address is mapped to a file in the current
   thread. */
bool
is_mapped (void *addr)
{
  return addr_to_map (addr) != NULL;
}

struct mapping *
addr_to_map (void *addr)
{
  struct hash_iterator i;
  hash_first (&i, &thread_current ()->file_map);
  while (hash_next (&i))
    {
      struct mapping *m = hash_entry (hash_cur (&i), struct mapping,
                                         elem);

      ASSERT (m != NULL);

      if (m->addr <= pg_round_down (addr) &&
          pg_round_down (addr) < m->addr + (m->num_pages * PGSIZE))
        return m;
    }
  return NULL;
}

static mapid_t
mmap (int fd, void *addr)
{
  /* Error if trying to map STDIO, or if the given address is outside the user
     space, NULL, or not page-aligned. */
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO || addr == NULL ||
      !is_user_vaddr (addr) || pg_ofs (addr) != 0)
    return -1;

  struct file *file = fd_to_file (fd);

  lock_filesystem ();
  int length = file_length (file);
  release_filesystem ();

  if (length == 0)
      return -1;

  /* Fail if the range of pages to be mapped (based on the given addr and file
     size) overlaps an already-mapped page, or spreads into kernel address
     space. */
  int offset;
  int num_pages = 0;
  for (offset = 0; offset < length; offset += PGSIZE)
    {
      if (!is_user_vaddr (addr + offset) ||
          pagedir_get_page (thread_current ()->pagedir, addr + offset) ||
          is_mapped (addr + offset))
        return -1;

      ++num_pages;
    }

  struct mapping *m = malloc (sizeof (struct mapping));
  if (m == NULL)
    PANIC ("Failed to allocate memory for file mapping.");

  m->mapid = thread_current ()->next_mapid++;

  lock_filesystem ();
  m->file = file_reopen (file);
  release_filesystem ();

  m->addr = addr;
  m->num_pages = num_pages;
  hash_insert (&thread_current ()->file_map, &m->elem);

  return m->mapid;
}

static void
munmap (mapid_t mapping, bool del_and_free)
{
  struct hash_elem *e = NULL;
  struct mapping *m = NULL;

  /* Have to loop through because we're searching by mapid, not the key
     (addr). */
  struct hash_iterator i;
  hash_first (&i, &thread_current ()->file_map);
  while (hash_next (&i))
    {
      e = hash_cur (&i);

      struct mapping *found = hash_entry (e, struct mapping, elem);
      ASSERT (found != NULL);

      if (found->mapid == mapping)
        {
          m = found;
          break;
        }
    }

  if (m != NULL)
    {
      struct file *f = m->file;

      /* Write any dirty pages back to the file. */
      int i;
      for (i = 0; i < m->num_pages; ++i)
        if (pagedir_is_dirty (thread_current ()->pagedir,
                              m->addr + (PGSIZE * i)))
          {
            lock_filesystem ();
            file_write_at (f, m->addr + (PGSIZE * i), PGSIZE, PGSIZE * i);
            release_filesystem ();
          }

      if (del_and_free)
        {
          hash_delete (&thread_current()->file_map, e);

          lock_filesystem ();
          file_close (f);
          release_filesystem ();

          free (m);
        }
    }
  else
    {
      /* mapid does not exist. */
      exit (-1);
    }
}
