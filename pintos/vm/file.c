/* file.c: Implementation of memory backed file object (mmaped object). */

#include "threads/mmu.h"
#include "vm/vm.h"
#include "filesys/filesys.h"
#include <stdlib.h>
#include <string.h>

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

static struct lock file_lock;
static struct lock map_lock;
static struct lock unmmap_lock;

static struct mmap_region
{
	void *start;
	size_t length;
	struct file *file;
	struct list_elem elem;
};

struct mmap_aux
{
	struct file *file;
	off_t offset;
	size_t read_bytes;
};

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
	lock_init(&map_lock);
	lock_init(&unmmap_lock);
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

static bool lazy_load_mmap(struct page *page, void *aux)
{
	struct mmap_aux *info = aux;
	struct file_page *file_page = &page->file;
	void *kva = page->frame->kva;

	file_page->file = info->file;
	file_page->file_offset = info->offset;
	file_page->file_size = info->read_bytes;

	lock_acquire(&map_lock);
	off_t bytes_read = file_read_at(info->file, kva, info->read_bytes, info->offset);
	lock_release(&map_lock);

	if (bytes_read != (off_t)info->read_bytes)
	{
		free(info);
		return false;
	}

	memset((uint8_t *)kva + info->read_bytes, 0, PGSIZE - info->read_bytes);
	free(info);
	return true;
}

static void mapping_fail(void *addr, int i)
{
	for (int j = 0; j < i; j++)
	{
		struct page *fail_page;
		fail_page = spt_find_page(&thread_current()->spt, (void *)((uint8_t *)addr + (PGSIZE * j)));
		spt_remove_page(&thread_current()->spt, fail_page);
	}
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset)
{
	if (pg_ofs(addr) != 0)
	{
		return NULL;
	}
	if (addr == NULL || length == 0 || !is_user_vaddr(addr))
	{
		return NULL;
	}
	size_t temp_length = length;
	int i = 0;
	struct mmap_region *mmaping = malloc(sizeof(struct mmap_region));
	if (mmaping == NULL)
	{
		return NULL;
	}
	file = file_reopen(file);
	if (file == NULL)
	{
		free(mmaping);
		return NULL;
	}
	mmaping->start = addr;
	mmaping->file = file;
	mmaping->length = length;
	list_push_back(&thread_current()->mmap_list, &mmaping->elem);
	off_t file_len = file_length(file);
	size_t file_remaining = 0;
	if (offset >= 0 && offset < file_len)
	{
		file_remaining = (size_t)(file_len - offset);
	}
	for (i; i < 1 + ((length - 1) / PGSIZE); i++)
	{
		if (spt_find_page(&thread_current()->spt, addr + (i * PGSIZE)) || offset % PGSIZE != 0 || length > KERN_BASE - (uintptr_t)addr || offset < 0)
		{
			list_remove(&mmaping->elem);
			mapping_fail(addr, i);
			free(mmaping);
			file_close(file);
			return NULL;
		}
		size_t page_bytes = temp_length >= PGSIZE ? PGSIZE : temp_length;
		size_t read_bytes = page_bytes < file_remaining ? page_bytes : file_remaining;
		struct mmap_aux *aux = malloc(sizeof *aux);
		if (aux == NULL)
		{
			list_remove(&mmaping->elem);
			mapping_fail(addr, i);
			free(mmaping);
			file_close(file);
			return NULL;
		}
		aux->file = file;
		aux->offset = offset;
		aux->read_bytes = read_bytes;

		if (!vm_alloc_page_with_initializer(VM_FILE, addr + (i * PGSIZE), writable, lazy_load_mmap, aux))
		{
			free(aux);
			list_remove(&mmaping->elem);
			mapping_fail(addr, i);
			free(mmaping);
			file_close(file);
			return NULL;
		}

		temp_length -= page_bytes;
		file_remaining -= read_bytes;
		if (file_remaining > 0)
			offset += PGSIZE;
	}
	return addr;
}

/* Do the munmap */
void do_munmap(void *addr)
{

	for (struct list_elem *mmap_elem = list_front(&thread_current()->mmap_list); mmap_elem != list_end(&thread_current()->mmap_list); mmap_elem = list_next(mmap_elem))
	{
		struct mmap_region *temp_mmap = list_entry(mmap_elem, struct mmap_region, elem);
		if (temp_mmap->start == addr)
		{
			struct file *will_close_file = temp_mmap->file;
			for (int i = 0; i < 1 + ((temp_mmap->length - 1) / PGSIZE); i++)
			{
				struct page *find_unmmap = spt_find_page(&thread_current()->spt, temp_mmap->start + (i * PGSIZE));
				spt_remove_page(&thread_current()->spt, find_unmmap);
			}
			list_remove(mmap_elem);
			free(temp_mmap);
			lock_acquire(&unmmap_lock);
			file_close(will_close_file);
			lock_release(&unmmap_lock);
			return;
		}
	}
	// 파일에 연결된 모든 페이지를 unmap을 해야됨
}
