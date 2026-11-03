/* file.c: Implementation of memory backed file object (mmaped object). */

#include "threads/mmu.h"
#include "vm/vm.h"
#include "filesys/filesys.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

static struct lock file_lock;

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
	lock_init(&file_lock);
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	file_page->file = NULL;
	file_page->file_size = 0;
	memset(kva, 0, PGSIZE);
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page = &page->file;
	if (file_page->file == NULL)
	{
		return false;
	}
	lock_acquire(&file_lock);
	off_t file_size = file_read_at(file_page->file, kva, file_page->file_size, file_page->file_offset);
	if (file_size != file_page->file_size)
	{
		lock_release(&file_lock);
		return false;
	}
	memset((uint8_t *)kva + file_size, 0, PGSIZE - file_size);
	lock_release(&file_lock);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page)
{
	struct file_page *file_page = &page->file;
	lock_acquire(&file_lock);
	if (pml4_is_dirty(page->owner_pml4, page->va))
	{
		off_t file_size = file_write_at(file_page->file, page->frame->kva, file_page->file_size, file_page->file_offset);
		if (file_size != file_page->file_size)
		{
			lock_release(&file_lock);
			return false;
		}
		file_page->file_size = file_size;
		pml4_set_dirty(page->owner_pml4, page->va, false);
	}
	pml4_clear_page(page->owner_pml4, page->va);
	page->frame->page = NULL;
	page->frame = NULL;
	lock_release(&file_lock);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy(struct page *page)
{
	ASSERT(page != NULL);
	struct file_page *file_page = &page->file;
	lock_acquire(&file_lock);
	if (page->frame != NULL && pml4_is_dirty(page->owner_pml4, page->va))
	{
		file_write_at(file_page->file, page->frame->kva, file_page->file_size, file_page->file_offset);
		// if(file_size!=file_page->file_size)
		// {
		// 	lock_release(&file_lock);
		// 	return;
		// }
		pml4_set_dirty(page->owner_pml4, page->va, false);
	}
	// file_close(file_page->file); mmap에서 사용하기
	lock_release(&file_lock);
	if (page->frame != NULL)
	{
		pml4_clear_page(page->owner_pml4, page->va);
		palloc_free_page(page->frame->kva);
		vm_remove_frame(page->frame);
		free(page->frame);
		page->frame = NULL;
	}
	file_page->file = NULL;
	file_page->file_size = 0;
	return;
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset)
{
}

/* Do the munmap */
void do_munmap(void *addr)
{
}
