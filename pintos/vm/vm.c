/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "list.h"
#include "threads/mmu.h"

#define STACK_MAX (1 << 20)

static uint64_t page_hash(const struct hash_elem *e, void *aux);
static bool page_less(const struct hash_elem *a,
					  const struct hash_elem *b, void *aux);
#define list_elem_to_hash_elem(LIST_ELEM) \
	list_entry(LIST_ELEM, struct hash_elem, list_elem)

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
									bool writable, vm_initializer *init,
									void *aux)
{
	ASSERT(VM_TYPE(type) != VM_UNINIT);

	struct supplemental_page_table *spt = &thread_current()->spt;

	if (spt_find_page(spt, upage) != NULL)
	{
		return false;
	}

	struct page *page = malloc(sizeof(struct page));
	if (page == NULL)
	{
		return false;
	}

	if (VM_TYPE(type) == VM_ANON)
	{
		uninit_new(page, upage, init, type, aux, anon_initializer);
	}
	else if (VM_TYPE(type) == VM_FILE)
	{
		uninit_new(page, upage, init, type, aux, file_backed_initializer);
	}
	else
	{
		free(page);
		return false;
	}

	page->writable = writable;

	if (!spt_insert_page(spt, page))
	{
		free(page);
		return false;
	}

	return true;
}
// TODO PAGE writable을 추가할수있도록
/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt, void *va)
{
	va = pg_round_down(va);
	uint64_t hash_return = hash_bytes(&va, sizeof va);
	uint64_t bucket_num = hash_return % (spt->pages.bucket_cnt);
	for (struct list_elem *i = list_begin(&spt->pages.buckets[bucket_num]); i != list_end(&spt->pages.buckets[bucket_num]); i = list_next(i))
	{
		struct hash_elem *hi = list_elem_to_hash_elem(i);
		struct page *seaching_page = hash_entry(hi, struct page, hash_elem);
		if (seaching_page->va == va)
		{
			return seaching_page;
		}
	}
	struct page *page = NULL;
	return page;
}
// 이승재 고침

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt,
					 struct page *page)
{
	int succ = false;
	/* TODO: Fill this function. */

	bool result = hash_insert(&spt->pages, &page->hash_elem);
	if (!result)
	{
		succ = true;
	}
	return succ;
}
void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	vm_dealloc_page(page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *victim UNUSED = vm_get_victim();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame(void)
{
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	frame = malloc(sizeof(struct frame));
	ASSERT(frame != NULL);
	frame->kva = palloc_get_page(PAL_USER);
	if (frame->kva == NULL)
	{
		free(frame);
		frame = vm_evict_frame();
		ASSERT(frame != NULL);
		return frame;
	}
	frame->page = NULL;
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user, bool write, bool not_present)
{
	if (!not_present || addr == NULL || !is_user_vaddr(addr))
	{
		return false;
	}
	struct supplemental_page_table *spt = &(thread_current()->spt);
	struct page *page = NULL;
	uintptr_t rsp;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	page = spt_find_page(spt, addr);
	if (page != NULL)
	{
		if (write && !page->writable)
		{
			return false;
		}
		return vm_do_claim_page(page);
	}
	if (user)
	{
		rsp = f->rsp;
	}
	else
	{
		rsp = thread_current()->user_rsp;
	}
	if (rsp == 0 || (uintptr_t)addr >= (uintptr_t)USER_STACK || (uintptr_t)addr < (uintptr_t)USER_STACK - STACK_MAX || ((uintptr_t)addr < (uintptr_t)rsp && (uintptr_t)rsp - (uintptr_t)addr > 8))
	{
		return false;
	}
	else
	{
		addr = pg_round_down(addr);
		vm_stack_growth(addr);
		return false; /* Stack growth is not implemented yet. */
	}
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
	{
		return false;
	}
	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	if (page == NULL)
	{
		return false;
	}

	struct frame *frame = vm_get_frame();

	if (frame == NULL)
	{
		return false;
	}

	/* Set links */
	frame->page = page;
	page->frame = frame;

	if (!pml4_set_page(thread_current()->pml4,
					   page->va,
					   frame->kva,
					   page->writable))
	{
		frame->page = NULL;
		page->frame = NULL;
		return false;
	}

	if (!swap_in(page, frame->kva))
	{
		pml4_clear_page(thread_current()->pml4, page->va);
		frame->page = NULL;
		page->frame = NULL;
		return false;
	}

	return true;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt)
{
	if (!hash_init(&spt->pages, page_hash, page_less, NULL))
	{
		PANIC("SPT hash initialization failed");
	} // 함수를 뭐 추가해야되는거지
	return;
}
// 이승재 고침
/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
}

uint64_t page_hash(const struct hash_elem *p, void *aux UNUSED)
{
	const struct page *page = hash_entry(p, struct page, hash_elem);
	return hash_bytes(&page->va, sizeof page->va);
}

bool page_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
	const struct page *page1 = hash_entry(a, struct page, hash_elem);
	const struct page *page2 = hash_entry(b, struct page, hash_elem);
	if ((uintptr_t)page1->va < (uintptr_t)page2->va)
	{
		return true;
	}
	return false;
}