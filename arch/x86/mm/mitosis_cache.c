#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/page-flags.h>
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
