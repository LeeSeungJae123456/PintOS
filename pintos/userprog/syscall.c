#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/palloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/init.h"
#include "devices/input.h"

#include "include/lib/kernel/stdio.h"

struct lock filesys_lock;

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

static void exit_helper(int status) NO_RETURN;
static size_t check_user_string(const char *str);
static bool check_user_buffer(const void *buffer, unsigned len, bool writable);
static struct file *lookup_file(int fd);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&(filesys_lock));
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
#ifdef VM
	thread_current()->user_rsp = f->rsp;
#endif
	switch (f->R.rax)
	{
	case SYS_HALT:
	{
		power_off();
		NOT_REACHED();
	}
	case SYS_EXIT:
	{
		exit_helper((int)f->R.rdi);
	}
	case SYS_FORK:
	{
		tid_t success;
		const char *t_name;

		t_name = (const char *)f->R.rdi;

		check_user_string(t_name);

		success = process_fork(t_name, f);

		/* 사용자 프로그램에 결과 전달 */
		f->R.rax = success;

		break;
	}
	case SYS_EXEC:
	{
		const char *cmd_line = (const char *)f->R.rdi;

		size_t size = check_user_string(cmd_line);
		char *f_name = palloc_get_page(0);
		if (f_name == NULL)
			exit_helper(-1);

		if (process_exec(memcpy(f_name, cmd_line, size + 1)) == -1)
			exit_helper(-1);
		break;
	}
	case SYS_WAIT:
	{
		int pid;
		int exit_status;

		/* wait 호출 후 반환값 기록*/
		pid = f->R.rdi;

		exit_status = process_wait(pid);
		f->R.rax = exit_status;
		break;
	}
	case SYS_CREATE:
	{
		const char *filename;
		unsigned init_size;

		filename = (const char *)f->R.rdi;
		init_size = f->R.rsi;

		check_user_string(filename);

		if (init_size > INT32_MAX)
		{
			f->R.rax = false;
			return;
		}

		lock_acquire(&filesys_lock);
		f->R.rax = filesys_create(filename, (off_t)init_size);
		lock_release(&filesys_lock);
		break;
	}
	case SYS_REMOVE:
	{
		const char *filename;

		filename = (const char *)f->R.rdi;
		check_user_string(filename);

		lock_acquire(&filesys_lock);
		f->R.rax = filesys_remove(filename);
		lock_release(&filesys_lock);
		break;
	}
	case SYS_OPEN:
	{
		struct file **curr_fd_table = thread_current()->fd_table;
		struct file *opened_file;
		const char *filename = (const char *)f->R.rdi;

		if (filename == NULL)
		{
			f->R.rax = -1;
			break;
		}
		check_user_string(filename);

		/* open 루틴 */
		for (size_t i = 2; i < PGSIZE / sizeof(*curr_fd_table); i++)
			if (curr_fd_table[i] == NULL)
			{

				lock_acquire(&filesys_lock);
				opened_file = filesys_open(filename);
				lock_release(&filesys_lock);
				if (opened_file == NULL)
				{
					f->R.rax = -1;
					return;
				}
				/* 반환 */
				curr_fd_table[i] = opened_file;
				f->R.rax = i;
				return;
			}
		f->R.rax = -1;
		break;
	}
	case SYS_FILESIZE:
	{
		struct file *file = lookup_file((int)f->R.rdi);
		f->R.rax = file == NULL ? -1 : file_length(file);
		break;
	}
	case SYS_READ:
	{
		int fd = f->R.rdi;
		char *buf = (char *)f->R.rsi;
		unsigned len = (off_t)f->R.rdx;

		if (len == 0)
		{
			f->R.rax = 0;
			return;
		}
		if (!check_user_buffer(buf, len, true))
		{
			f->R.rax = -1;
			return;
		}

		if (fd == 1)
		{
			f->R.rax = -1;
			return;
		}
		if (fd == 0)
		{
			for (size_t i = 0; i < len; i++)
				buf[i] = input_getc();
			f->R.rax = len;
		}
		else
		{
			struct file *file = lookup_file(fd);
			if (file == NULL)
			{
				f->R.rax = -1;
				return;
			}

			lock_acquire(&filesys_lock);
			f->R.rax = (uint64_t)file_read(file, buf, len);
			lock_release(&filesys_lock);
		}
		break;
	}
	case SYS_WRITE:
	{
		int fd = f->R.rdi;
		const char *buf = (const char *)f->R.rsi;
		unsigned len = f->R.rdx;

		if (len == 0)
		{
			f->R.rax = 0;
			return;
		}
		if (!check_user_buffer(buf, len, false))
		{
			f->R.rax = -1;
			return;
		}

		if (fd == 0)
		{
			f->R.rax = -1;
			return;
		}
		if (fd == 1)
		{
			putbuf(buf, len);
			f->R.rax = len;
			return;
		}

		struct file *file = lookup_file(fd);
		if (file == NULL)
		{
			f->R.rax = -1;
			return;
		}

		lock_acquire(&filesys_lock);
		f->R.rax = (uint64_t)file_write(file, buf, len);
		lock_release(&filesys_lock);
		return;
	}
	case SYS_SEEK:
	{
		int fd;
		unsigned pos;

		fd = (int)f->R.rdi;
		pos = (unsigned)f->R.rsi;

		struct file *file = lookup_file(fd);
		if (file == NULL)
			return;

		if (pos > INT32_MAX)
			return;

		file_seek(file, (off_t)pos);
		break;
	}
	case SYS_TELL:
	{
		int fd;

		fd = (int)f->R.rdi;

		struct file *file = lookup_file(fd);
		if (file == NULL)
		{
			f->R.rax = -1;
			return;
		}

		f->R.rax = file_tell(file);
		break;
	}
	case SYS_CLOSE:
	{
		int fd = (int)f->R.rdi;
		struct file *file = lookup_file(fd);
		if (file != NULL)
		{
			lock_acquire(&filesys_lock);
			file_close(file);
			lock_release(&filesys_lock);
			thread_current()->fd_table[fd] = NULL;
		}
		break;
	}
	default:
	{
		break;
	}
	}
}

/* 종료 문자까지 검증하며, 기존 명령줄 한 페이지 제한을 유지합니다. */
static size_t check_user_string(const char *str)
{
	struct thread *curr = thread_current();
	if (str == NULL)
		exit_helper(-1);

	for (size_t len = 0; len < PGSIZE; len++)
	{
		const char *pos = str + len;
		if (!is_user_vaddr(pos) || pml4_get_page(curr->pml4, pos) == NULL)
			exit_helper(-1);
		if (*pos == '\0')
			return len;
	}
	exit_helper(-1);
}

/* writable은 커널이 사용자 버퍼에 쓰는 READ에서만 요구합니다. */
static bool check_user_buffer(const void *buffer, unsigned len, bool writable)
{
	if (len == 0)
		return true;
	if (buffer == NULL || !is_user_vaddr(buffer))
		exit_helper(-1);
	if (len > (uintptr_t)KERN_BASE - (uintptr_t)buffer)
		return false;

	struct thread *curr = thread_current();
	uintptr_t last = (uintptr_t)pg_round_down((uintptr_t)buffer + len - 1);
	for (uintptr_t pg = (uintptr_t)pg_round_down(buffer); pg <= last; pg += PGSIZE)
	{
		if (pml4_get_page(curr->pml4, (const void *)pg) == NULL)
			exit_helper(-1);
		uint64_t *pte = pml4e_walk(curr->pml4, pg, 0);
		if (!is_user_pte(pte) || (writable && !is_writable(pte)))
			exit_helper(-1);
	}
	return true;
}

static struct file *lookup_file(int fd)
{
	struct file **table = thread_current()->fd_table;
	if (fd < 2 || (size_t)fd >= PGSIZE / sizeof(*table) || table == NULL)
		return NULL;
	return table[fd];
}

/* 임시 종료 헬퍼 함수입니다.
 * 추후 커널에서의 종료 계약이 완료되면, 대체할 수 있습니다. */
static void exit_helper(int status)
{
	struct thread *curr = thread_current();

	if (curr->own_status != NULL)
		curr->own_status->exit_status = status;

	thread_exit();
}