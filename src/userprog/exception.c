#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "userprog/syscall.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include <string.h>
#include "userprog/process.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void)
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");\
}

/* Prints exception statistics. */
void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f)
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  /* The interrupt frame's code segment value tells us where the
     exception originated. */

  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      exit (-1);

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to task 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f)
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  if (pg_round_down (fault_addr) == NULL || !not_present)
    {
      exit (-1);
      /* TODO: not entirely happy about this. Should we not be printing a
               message and using kill like the original failure code? */
    }

  struct thread *cur = thread_current ();

  struct sup_page *page = get_sup_page (&cur->supp_pt,
                                        pg_round_down(fault_addr));

  if (page != NULL && !page->loaded && is_user_vaddr(fault_addr))
    {
      void *frame = NULL;
      if (!page->is_swapped)
        {
          /* Page data is in the file system */
          frame = allocate_frame (PAL_USER);

          /* filesystem lock will only be acquired if current thread does not
             hold it. This prevents issues when coming from a read syscall */
          lock_filesystem ();
          file_seek (page->file, page->offset);
          file_read (page->file, frame, page->read_bytes);
          release_filesystem ();

          pin_by_addr (frame);
          memset (frame + page->read_bytes, 0, page->zero_bytes);
          unpin_by_addr (frame);

          lock_acquire (&cur->pd_lock);
          if (!pagedir_set_page (cur->pagedir, page->user_addr, frame,
                                 page->writable))
            free_frame (frame);

          lock_release (&cur->pd_lock);
          page->loaded = true;
        }
      else if (page->is_swapped)
        {
          /* Page data is in a swap slot */
          frame = allocate_frame (PAL_USER);

          lock_acquire (&cur->pd_lock);
          if (!pagedir_set_page (cur->pagedir, page->user_addr, frame,
                                 page->writable))
            free_frame (frame);

          lock_release (&cur->pd_lock);

          pin_by_addr (frame);
          free_slot (frame, page->swap_index);
          unpin_by_addr (frame);
          page->loaded = true;
          page->is_swapped = false;
        }
    }
  else
    {
      ASSERT (page == NULL); /* This might not be right. */
      page = create_sup_page (NULL, 0, PGSIZE, true,
                              pg_round_down (fault_addr), 0);

      struct hash_iterator i;
      hash_first (&i, &cur->file_map);
      while (hash_next (&i))
        {
          struct mapid_node *m = hash_entry (hash_cur (&i), struct mapid_node,
                                             elem);

          ASSERT (m != NULL);

          /* This mapping is of no interest to us - no point doing further
             inspection. */
          if (m->addr < pg_round_down (fault_addr))
            continue;

          if (m->addr == pg_round_down (fault_addr))
            {
              /* Address is mapped to a file. */
              void *frame = allocate_frame (PAL_USER | PAL_ZERO);

              /* TODO: for mmap-read test, we already hold the filesystem lock.
                      Will this always be the case? */

              file_read (m->file, frame, PGSIZE);

              lock_acquire (&cur->pd_lock);
              pagedir_set_page (cur->pagedir, page->user_addr, frame,
                                page->writable);
              lock_release (&cur->pd_lock);
              page->loaded = true;
              return;
            }
          else if (pg_round_down (fault_addr) <=
                   m->addr + (m->num_pages * PGSIZE))
            {
              /* Accessing a memory mapped file somewhere in its range, but
                 not in it's first page. I think this needs implementing? */
              NOT_REACHED ();
            }
        }

      /* No suitable mem->file map found. This is a legit page fault, die. */
      printf ("Page fault at %p: %s error %s page in %s context.\n",
              fault_addr,
              not_present ? "not present" : "rights violation",
              write ? "writing" : "reading",
              user ? "user" : "kernel");
      kill (f);
    }

}
