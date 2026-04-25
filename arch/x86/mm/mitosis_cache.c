#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/page-flags.h>
#include <asm/pgtable_repl.h>

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
EXPORT_SYMBOL(mitosis_cache);

bool mitosis_cache_push(struct page *page, int node, int level)
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
	atomic64_inc(&cache->returns);
	spin_unlock_irqrestore(&cache->lock, flags);

	return true;
}
EXPORT_SYMBOL(mitosis_cache_push);

struct page *mitosis_cache_pop(int node, int level)
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
		atomic64_inc(&cache->misses);
		return NULL;
	}
	cache->head = page->pt_replica;
	atomic_dec(&cache->count);
	atomic64_inc(&cache->hits);
	spin_unlock_irqrestore(&cache->lock, flags);

	page->pt_replica = NULL;
	SetPageMitosisFromCache(page);

	clear_highpage(page);

	return page;
}
EXPORT_SYMBOL(mitosis_cache_pop);

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
EXPORT_SYMBOL(mitosis_cache_drain_node);


int mitosis_cache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		total += mitosis_cache_drain_node(node);
	}

	return total;
}
EXPORT_SYMBOL(mitosis_cache_drain_all);

void mitosis_defer_pte_page_free(struct mm_struct *mm, struct page *page)
{
    unsigned long flags;

    WRITE_ONCE(page->pt_replica, NULL);
    pagetable_dtor(page_ptdesc(page));
    page->pt_owner_mm = NULL;

    if (!mm) {
        ClearPageMitosisFromCache(page);
        __free_page(page);
        return;
    }

    spin_lock_irqsave(&mm->mitosis_deferred_lock, flags);
    page->pt_replica = mm->mitosis_deferred_pages;
    mm->mitosis_deferred_pages = page;
    spin_unlock_irqrestore(&mm->mitosis_deferred_lock, flags);
}
EXPORT_SYMBOL(mitosis_defer_pte_page_free);

void mitosis_drain_deferred_pages(struct mm_struct *mm)
{
    struct page *page, *next;
    unsigned long flags;

    if (!mm || !READ_ONCE(mm->mitosis_deferred_pages))
        return;

    spin_lock_irqsave(&mm->mitosis_deferred_lock, flags);
    page = mm->mitosis_deferred_pages;
    mm->mitosis_deferred_pages = NULL;
    spin_unlock_irqrestore(&mm->mitosis_deferred_lock, flags);

    while (page) {
        int nid = page_to_nid(page);
        bool from_cache = PageMitosisFromCache(page);
        next = page->pt_replica;
        page->pt_replica = NULL;
        ClearPageMitosisFromCache(page);
        if (from_cache && mitosis_cache_push(page, nid, MITOSIS_CACHE_PTE)) {
            page = next;
            continue;
        }
        __free_page(page);
        page = next;
    }
}
EXPORT_SYMBOL(mitosis_drain_deferred_pages);
