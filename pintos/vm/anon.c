/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "include/threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_bitmap;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

static struct lock swap_lock;
static struct lock bitmap_lock;
/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);
	ASSERT(swap_disk != NULL);
	disk_sector_t num = disk_size(swap_disk);
	swap_bitmap = bitmap_create(num / (PGSIZE / DISK_SECTOR_SIZE));
	ASSERT(swap_bitmap != NULL);
	lock_init(&swap_lock);
	lock_init(&bitmap_lock);
	return;
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &anon_ops;
	page->anon.page_index = BITMAP_ERROR;
	memset(kva, 0, PGSIZE);
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;
	size_t bitmap_index = page->anon.page_index;
	swap_disk = disk_get(1, 1);
	lock_acquire(&bitmap_lock);
	if (bitmap_index >= bitmap_size(swap_bitmap))
	{
		lock_release(&bitmap_lock);
		return false;
	}
	bool test = bitmap_test(swap_bitmap, bitmap_index);
	if (test == false)
	{
		lock_release(&bitmap_lock);
		return false;
	}
	for (int i = 0; i < (PGSIZE / DISK_SECTOR_SIZE); i++)
	{
		disk_read(swap_disk, bitmap_index * (PGSIZE / DISK_SECTOR_SIZE) + i, (uint8_t *)kva + i * DISK_SECTOR_SIZE);
	}
	bitmap_set(swap_bitmap, bitmap_index, false);
	page->anon.page_index = BITMAP_ERROR;
	lock_release(&bitmap_lock);
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
	swap_disk = disk_get(1, 1);
	lock_acquire(&bitmap_lock);
	size_t index = bitmap_scan(swap_bitmap, 0, 1, false);
	if (index == BITMAP_ERROR)
	{
		lock_release(&bitmap_lock);
		return false;
	}
	for (int i = 0; i < (PGSIZE / DISK_SECTOR_SIZE); i++)
	{
		disk_write(swap_disk, index * (PGSIZE / DISK_SECTOR_SIZE) + i, (uint8_t *)page->frame->kva + i * DISK_SECTOR_SIZE);
	}
	bitmap_set(swap_bitmap, index, true);
	lock_release(&bitmap_lock);
	// vm_evicit후 재판단TODO
	pml4_clear_page(page->owner_pml4, page->va);
	// vm_evcit후 재판단TODO
	page->anon.page_index = index;

	// vm_evcit후 재판단TODO
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy(struct page *page)
{
	ASSERT(page != NULL);
	if (page->frame != NULL)
	{
		vm_remove_frame(page->frame);
		palloc_free_page(page->frame->kva);
		free(page->frame);
		page->frame = NULL;
		// struct anon_page *anon_page = &page->anon;
	}
	if (page->anon.page_index != BITMAP_ERROR)
	{
		lock_acquire(&bitmap_lock);
		bitmap_set(swap_bitmap, page->anon.page_index, false);
		lock_release(&bitmap_lock);
		page->anon.page_index = BITMAP_ERROR;
	}
}