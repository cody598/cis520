#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "devices/input.h"

struct file_descripton
{
  int fd_number;
  tid_t owner;
  struct file *file_struct;
  struct list_elem element;
};

/* a list of open files, represents all the files open by the user process
   through syscalls. */
struct list open_files; 

/* the lock used by syscalls involving file system to ensure only one thread
   at a time is accessing file system */
struct lock fs_lock;

static void syscall_handler (struct intr_frame *);

/* System call functions */
static void halt (void);
static void exit (int);
static pid_t exec (const char *);
static int wait (pid_t);
static bool create (const char*, unsigned);
static bool remove (const char *);
static int open (const char *);
static int filesize (int);
static int read (int, void *, unsigned);
static int write (int, const void *, unsigned);
static void seek (int, unsigned);
static unsigned tell (int);
static void close (int);
/* End of system call functions */

static struct file_descripton *get_open_file (int);
static void close_open_file (int);
bool is_valid_ptr (const void *);
static int allocate_fd (void);
void close_file_by_owner (tid_t);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&open_files);
  lock_init (&fs_lock);
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t *esp;
  esp = f->esp;

  if (!is_valid_ptr (esp) || !is_valid_ptr (esp + 1) ||
      !is_valid_ptr (esp + 2) || !is_valid_ptr (esp + 3))
    {
      exit (-1);
    }
  else
    {
      int syscall_number = *esp;
      switch (syscall_number)
        {
        case SYS_HALT:
          halt ();
          break;
        case SYS_EXIT:
          exit (*(esp + 1));
          break;
        case SYS_EXEC:
          f->eax = exec ((char *) *(esp + 1));
          break;
        case SYS_WAIT:
          f->eax = wait (*(esp + 1));
          break;
        case SYS_CREATE:
          f->eax = create ((char *) *(esp + 1), *(esp + 2));
          break;
        case SYS_REMOVE:
          f->eax = remove ((char *) *(esp + 1));
          break;
        case SYS_OPEN:
          f->eax = open ((char *) *(esp + 1));
          break;
        case SYS_FILESIZE:
	  f->eax = filesize (*(esp + 1));
	  break;
        case SYS_READ:
          f->eax = read (*(esp + 1), (void *) *(esp + 2), *(esp + 3));
          break;
        case SYS_WRITE:
          f->eax = write (*(esp + 1), (void *) *(esp + 2), *(esp + 3));
          break;
        case SYS_SEEK:
          seek (*(esp + 1), *(esp + 2));
          break;
        case SYS_TELL:
          f->eax = tell (*(esp + 1));
          break;
        case SYS_CLOSE:
          close (*(esp + 1));
          break;
        default:
          break;
        }
    }
}


/* Terminates the current user program, returning status to the kernel.*/
/* Exits the thread due to invalid addresses */
void
exit (int pointerStatus)
{
  struct child_status *child;
  struct thread *current = thread_current ();
  printf ("%s: exit(%d)\n", current->name, pointerStatus);
  struct thread *parent = thread_get_by_id (current->parent_id);
  if (parent != NULL) 
    {
      struct list_elem *elem = list_tail(&parent->children);
      while ((elem = list_prev (elem)) != list_head (&parent->children))
        {
          child = list_entry (elem, struct child_status, elem_status);
          if (child->id == current->tid)
          {
            lock_acquire (&parent->lock_child);
            child->exit_called = true;
            child->exit_status = pointerStatus;
            lock_release (&parent->lock_child);
          }
        }
    }
  thread_exit ();
}

void
halt (void)
{
  shutdown_power_off ();
}

pid_t
exec (const char *cmd_line)
{
  /* a thread's id. When there is a user process within a kernel thread, we
   * use one-to-one mapping from tid to pid, which means pid = tid
   */
  tid_t tid;
  struct thread *current;
  /* check if the user pinter is valid */
  if (!is_valid_ptr (cmd_line))
    {
      exit (-1);
    }

  current = thread_current ();

  current->child_load_status = 0;
  tid = process_execute (cmd_line);
  lock_acquire(&current->lock_child);
  while (current->child_load_status == 0)
    cond_wait(&current->cond_child, &current->lock_child);
  if (current->child_load_status == -1)
    tid = -1;
  lock_release(&current->lock_child);
  return tid;
}

int 
wait (pid_t pid)
{ 
  return process_wait(pid);
}

bool
create (const char *file_name, unsigned size)
{
  bool status;

  if (!is_valid_ptr (file_name))
    exit (-1);

  lock_acquire (&fs_lock);
  status = filesys_create(file_name, size);  
  lock_release (&fs_lock);
  return status;
}

bool 
remove (const char *file_name)
{
  bool status;
  if (!is_valid_ptr (file_name))
    exit (-1);

  lock_acquire (&fs_lock);  
  status = filesys_remove (file_name);
  lock_release (&fs_lock);
  return status;
}

int
open (const char *file_name)
{
  struct file *f;
  struct file_descripton *fd;
  int status = -1;
  
  if (!is_valid_ptr (file_name))
    exit (-1);

  lock_acquire (&fs_lock); 
 
  f = filesys_open (file_name);
  if (f != NULL)
    {
      fd = calloc (1, sizeof *fd);
      fd->fd_number = allocate_fd ();
      fd->owner = thread_current ()->tid;
      fd->file_struct = f;
      list_push_back (&open_files, &fd->element);
      status = fd->fd_number;
    }
  lock_release (&fs_lock);
  return status;
}

int
filesize (int fd)
{
  struct file_descripton *fd_struct;
  int status = -1;
  lock_acquire (&fs_lock); 
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL)
    status = file_length (fd_struct->file_struct);
  lock_release (&fs_lock);
  return status;
}

int
read (int fd, void *buffer, unsigned size)
{
  struct file_descripton *fd_struct;
  int status = 0; 

  if (!is_valid_ptr (buffer) || !is_valid_ptr (buffer + size - 1))
    exit (-1);

  lock_acquire (&fs_lock); 
  
  if (fd == STDOUT_FILENO)
    {
      lock_release (&fs_lock);
      return -1;
    }

  if (fd == STDIN_FILENO)
    {
      uint8_t c;
      unsigned counter = size;
      uint8_t *buf = buffer;
      while (counter > 1 && (c = input_getc()) != 0)
        {
          *buf = c;
          buffer++;
          counter--; 
        }
      *buf = 0;
      lock_release (&fs_lock);
      return (size - counter);
    } 
  
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL)
    status = file_read (fd_struct->file_struct, buffer, size);

  lock_release (&fs_lock);
  return status;
}

int
write (int fd, const void *buffer, unsigned size)
{
  struct file_descripton *fd_struct;  
  int status = 0;

  if (!is_valid_ptr (buffer) || !is_valid_ptr (buffer + size - 1))
    exit (-1);

  lock_acquire (&fs_lock); 

  if (fd == STDIN_FILENO)
    {
      lock_release(&fs_lock);
      return -1;
    }

  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, size);
      lock_release(&fs_lock);
      return size;
    }
 
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL)
    status = file_write (fd_struct->file_struct, buffer, size);
  lock_release (&fs_lock);
  return status;
}


void 
seek (int fd, unsigned position)
{
  struct file_descripton *fd_struct;
  lock_acquire (&fs_lock); 
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL)
    file_seek (fd_struct->file_struct, position);
  lock_release (&fs_lock);
  return ;
}

unsigned 
tell (int fd)
{
  struct file_descripton *fd_struct;
  int status = 0;
  lock_acquire (&fs_lock); 
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL)
    status = file_tell (fd_struct->file_struct);
  lock_release (&fs_lock);
  return status;
}

void 
close (int fd)
{
  struct file_descripton *fd_struct;
  lock_acquire (&fs_lock); 
  fd_struct = get_open_file (fd);
  if (fd_struct != NULL && fd_struct->owner == thread_current ()->tid)
    close_open_file (fd);
  lock_release (&fs_lock);
  return ; 
}

struct file_descripton *
get_open_file (int fd)
{
  struct list_elem *elem;
  struct file_descripton *fd_struct; 
  elem = list_tail (&open_files);
  while ((elem = list_prev (elem)) != list_head (&open_files)) 
    {
      fd_struct = list_entry (elem, struct file_descripton, element);
      if (fd_struct->fd_number == fd)
	return fd_struct;
    }
  return NULL;
}

void
close_open_file (int fd)
{
  struct list_elem *elem;
  struct list_elem *prev;
  struct file_descripton *fd_struct; 
  elem = list_end (&open_files);
  while (elem != list_head (&open_files)) 
    {
      prev = list_prev (elem);
      fd_struct = list_entry (elem, struct file_descripton, element);
      if (fd_struct->fd_number == fd)
	{
	  list_remove (elem);
          file_close (fd_struct->file_struct);
	  free (fd_struct);
	  return ;
	}
      elem = prev;
    }
  return ;
}


/* The kernel must be very careful about doing so, because the user can
 * pass a null pointer, a pointer to unmapped virtual memory, or a pointer
 * to kernel virtual address space (above PHYS_BASE). All of these types of
 * invalid pointers must be rejected without harm to the kernel or other
 * running processes, by terminating the offending process and freeing
 * its resources.
 */
bool
is_valid_ptr (const void *usr_ptr)
{
  struct thread *current = thread_current ();
  if (usr_ptr != NULL && is_user_vaddr (usr_ptr))
    {
      return (pagedir_get_page (current->pagedir, usr_ptr)) != NULL;
    }
  return false;
}

int
allocate_fd ()
{
  static int fd_current = 1;
  return ++fd_current;
}

void
close_file_by_owner (tid_t tid)
{
  struct list_elem *elem;
  struct list_elem *next;
  struct file_descripton *fd_struct; 
  elem = list_begin (&open_files);
  while (elem != list_tail (&open_files)) 
    {
      next = list_next (elem);
      fd_struct = list_entry (elem, struct file_descripton, element);
      if (fd_struct->owner == tid)
	{
	  list_remove (elem);
	  file_close (fd_struct->file_struct);
          free (fd_struct);
	}
      elem = next;
    }
}
