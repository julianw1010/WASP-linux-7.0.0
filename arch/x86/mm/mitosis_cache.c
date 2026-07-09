#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/page-flags.h>
#include <asm/tlb.h>
#include <asm/mitosis.h>

struct mitosis_cache_head mitosis_cache[NUMA_NODE_COUNT] = {
	[0 ... NUMA_NODE_COUNT - 1] = {
		.lock		= __SPIN_LOCK_UNLOCKED(mitosis_cache.lock),
		.head		= NULL,
		.count		= ATOMIC_INIT(0),
		.hits		= ATOMIC64_INIT(0),
		.misses		= ATOMIC64_INIT(0),
		.returns	= ATOMIC64_INIT(0),
	}
};

static bool mitosis_cache_counted(struct mm_struct *owner_mm)
{
	return owner_mm && (owner_mm->repl_pgd_enabled || owner_mm->cache_only_mode);
}

bool mitosis_cache_push(struct page *page, int node, int level,
			struct mm_struct *owner_mm)
{
	struct mitosis_cache_head *cache;
	unsigned long flags;

	(void)level;

	ClearPageMitosisFromCache(page);
	page->pt_owner_mm = NULL;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return false;

	cache = &mitosis_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page->pt_replica = cache->head;
	cache->head = page;
	atomic_inc(&cache->count);
	if (mitosis_cache_counted(owner_mm))
		atomic64_inc(&cache->returns);
	spin_unlock_irqrestore(&cache->lock, flags);

	return true;
}

struct page *mitosis_cache_pop(int node, int level, struct mm_struct *owner_mm)
{
	struct mitosis_cache_head *cache;
	struct page *page;
	unsigned long flags;

	(void)level;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return NULL;

	cache = &mitosis_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	if (!page) {
		spin_unlock_irqrestore(&cache->lock, flags);
		if (mitosis_cache_counted(owner_mm))
			atomic64_inc(&cache->misses);
		return NULL;
	}
	cache->head = page->pt_replica;
	atomic_dec(&cache->count);
	if (mitosis_cache_counted(owner_mm))
		atomic64_inc(&cache->hits);
	spin_unlock_irqrestore(&cache->lock, flags);

	page->pt_replica = NULL;
	SetPageMitosisFromCache(page);

	clear_highpage(page);
	return page;
}

int mitosis_cache_drain_node(int node)
{
	struct mitosis_cache_head *cache;
	struct page *page, *next;
	unsigned long flags;
	int freed = 0;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return 0;

	cache = &mitosis_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	cache->head = NULL;
	atomic_set(&cache->count, 0);
	spin_unlock_irqrestore(&cache->lock, flags);

	while (page) {
		next = page->pt_replica;
		page->pt_replica = NULL;
		page->pt_owner_mm = NULL;
		ClearPageMitosisFromCache(page);
		__free_page(page);
		freed++;
		page = next;
	}

	return freed;
}

int mitosis_cache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		total += mitosis_cache_drain_node(node);
	}

	return total;
}

void mitosis_cache_count_return(struct mm_struct *owner_mm, int node)
{
	if (node < 0 || node >= NUMA_NODE_COUNT)
		return;

	if (mitosis_cache_counted(owner_mm))
		atomic64_inc(&mitosis_cache[node].returns);
}

void mitosis_cache_defer(struct mmu_gather *tlb, struct page *page)
{
	page->pt_replica = tlb->mitosis_deferred_cache;
	tlb->mitosis_deferred_cache = page;
}

void mitosis_cache_defer_drain(struct mmu_gather *tlb)
{
	struct page *page = tlb->mitosis_deferred_cache;

	tlb->mitosis_deferred_cache = NULL;

	while (page) {
		struct page *next = page->pt_replica;
		struct mm_struct *owner_mm = page->pt_owner_mm;

		page->pt_replica = NULL;
		if (!mitosis_cache_push(page, page_to_nid(page), 0, owner_mm))
			__free_page(page);
		page = next;
	}
}

bool mitosis_cache_return_table(struct ptdesc *ptdesc)
{
	struct page *page = ptdesc_page(ptdesc);

	if (!PageMitosisFromCache(page))
		return false;

	pagetable_dtor(ptdesc);

	if (!mitosis_cache_push(page, page_to_nid(page), 0, NULL))
		__free_page(page);

	return true;
}
