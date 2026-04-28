// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/pgtable_repl.h>
#include <asm/io.h>
#include <asm/cacheflush.h>

struct page *mitosis_alloc_replica_page(int node, int order)
{
	struct page *page;
	int dummy_level = 0;

	if (order == 0) {
		page = mitosis_cache_pop(node, dummy_level);
		if (page) {
			if (WARN_ON_ONCE(page_to_nid(page) != node)) {
				__free_page(page);
				return NULL;
			}
			return page;
		}
	}

	page = alloc_pages_node(node,
		GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, order);

	if (WARN_ON_ONCE(!page))
		return NULL;
	if (WARN_ON_ONCE(page_to_nid(page) != node)) {
		__free_pages(page, order);
		return NULL;
	}

	return page;
}
EXPORT_SYMBOL(mitosis_alloc_replica_page);

int alloc_pte_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count)
{
	int i;
	int base_node;
	int expected_count;
	nodemask_t nodes_snapshot;

	if (!base_page || !mm || !pages || !count)
		return -EINVAL;

	*count = 0;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return -EAGAIN;

	nodes_snapshot = mm->repl_pgd_nodes;
	expected_count = nodes_weight(nodes_snapshot);
	if (expected_count < 2 || expected_count > NUMA_NODE_COUNT)
		return -EAGAIN;

	base_node = page_to_nid(base_page);
	if (!node_isset(base_node, nodes_snapshot))
		node_set(base_node, nodes_snapshot);

	base_page->pt_owner_mm = mm;
	pages[0] = base_page;
	*count = 1;

	for_each_node_mask(i, nodes_snapshot) {
		struct page *new_page;

		if (i == base_node)
			continue;

		new_page = mitosis_alloc_replica_page(i, 0);
		if (!new_page)
			goto fail;

		if (WARN_ON_ONCE(!pagetable_pte_ctor(mm, page_ptdesc(new_page)))) {
			__free_page(new_page);
			goto fail;
		}

		new_page->pt_owner_mm = mm;
		mm_inc_nr_ptes(mm);
		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;

fail:
	for (i = 1; i < *count; i++) {
		pagetable_dtor(page_ptdesc(pages[i]));
		mm_dec_nr_ptes(mm);
		pages[i]->pt_owner_mm = NULL;
		__free_page(pages[i]);
	}
	*count = 0;
	return -ENOMEM;
}

int alloc_pmd_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count)
{
	int i;
	int base_node;
	int expected_count;
	nodemask_t nodes_snapshot;

	if (!base_page || !mm || !pages || !count)
		return -EINVAL;

	*count = 0;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return -EAGAIN;

	nodes_snapshot = mm->repl_pgd_nodes;
	expected_count = nodes_weight(nodes_snapshot);
	if (expected_count < 2 || expected_count > NUMA_NODE_COUNT)
		return -EAGAIN;

	base_node = page_to_nid(base_page);
	if (!node_isset(base_node, nodes_snapshot))
		node_set(base_node, nodes_snapshot);

	base_page->pt_owner_mm = mm;
	pages[0] = base_page;
	*count = 1;

	for_each_node_mask(i, nodes_snapshot) {
		struct page *new_page;

		if (i == base_node)
			continue;

		new_page = mitosis_alloc_replica_page(i, 0);
		if (!new_page)
			goto fail;

		if (WARN_ON_ONCE(!pagetable_pmd_ctor(mm, page_ptdesc(new_page)))) {
			__free_page(new_page);
			goto fail;
		}

		new_page->pt_owner_mm = mm;
		mm_inc_nr_pmds(mm);
		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;

fail:
	for (i = 1; i < *count; i++) {
		pagetable_dtor(page_ptdesc(pages[i]));
		mm_dec_nr_pmds(mm);
		pages[i]->pt_owner_mm = NULL;
		__free_page(pages[i]);
	}
	*count = 0;
	return -ENOMEM;
}

int alloc_pud_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count)
{
	int i;
	int base_node;
	int expected_count;
	nodemask_t nodes_snapshot;

	if (!base_page || !mm || !pages || !count)
		return -EINVAL;

	*count = 0;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return -EAGAIN;

	nodes_snapshot = mm->repl_pgd_nodes;
	expected_count = nodes_weight(nodes_snapshot);
	if (expected_count < 2 || expected_count > NUMA_NODE_COUNT)
		return -EAGAIN;

	base_node = page_to_nid(base_page);
	if (!node_isset(base_node, nodes_snapshot))
		node_set(base_node, nodes_snapshot);

	base_page->pt_owner_mm = mm;
	pages[0] = base_page;
	*count = 1;

	for_each_node_mask(i, nodes_snapshot) {
		struct page *new_page;

		if (i == base_node)
			continue;

		new_page = mitosis_alloc_replica_page(i, 0);
		if (!new_page)
			goto fail;

		new_page->pt_owner_mm = mm;
		mm_inc_nr_puds(mm);
		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;

fail:
	for (i = 1; i < *count; i++) {
		mm_dec_nr_puds(mm);
		pages[i]->pt_owner_mm = NULL;
		__free_page(pages[i]);
	}
	*count = 0;
	return -ENOMEM;
}

int alloc_p4d_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count)
{
	int i;
	int base_node;
	int expected_count;
	nodemask_t nodes_snapshot;

	if (!base_page || !mm || !pages || !count)
		return -EINVAL;

	*count = 0;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return -EAGAIN;

	nodes_snapshot = mm->repl_pgd_nodes;
	expected_count = nodes_weight(nodes_snapshot);
	if (expected_count < 2 || expected_count > NUMA_NODE_COUNT)
		return -EAGAIN;

	base_node = page_to_nid(base_page);
	if (!node_isset(base_node, nodes_snapshot))
		node_set(base_node, nodes_snapshot);

	base_page->pt_owner_mm = mm;
	pages[0] = base_page;
	*count = 1;

	for_each_node_mask(i, nodes_snapshot) {
		struct page *new_page;

		if (i == base_node)
			continue;

		new_page = mitosis_alloc_replica_page(i, 0);
		if (!new_page)
			goto fail;

		new_page->pt_owner_mm = mm;
		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;

fail:
	for (i = 1; i < *count; i++) {
		pages[i]->pt_owner_mm = NULL;
		__free_page(pages[i]);
	}
	*count = 0;
	return -ENOMEM;
}

int alloc_pgd_replicas(struct page *base_page, struct mm_struct *mm,
		       nodemask_t nodes, struct page **pages, int *count)
{
	int i;
	int base_node;
	int expected_count;
	int alloc_order = mitosis_pgd_alloc_order();

	if (!base_page || !pages || !count)
		return -EINVAL;

	*count = 0;
	expected_count = nodes_weight(nodes);
	if (expected_count < 2 || expected_count > NUMA_NODE_COUNT)
		return -EINVAL;

	base_node = page_to_nid(base_page);
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	base_page->pt_owner_mm = mm;
	pages[0] = base_page;
	*count = 1;

	for_each_node_mask(i, nodes) {
		struct page *new_page;

		if (i == base_node)
			continue;

		new_page = mitosis_alloc_replica_page(i, alloc_order);
		if (!new_page)
			goto fail;

		new_page->pt_owner_mm = mm;
		if (mm)
			WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;

fail:
	for (i = 1; i < *count; i++) {
		pages[i]->pt_owner_mm = NULL;
		__free_pages(pages[i], alloc_order);
	}
	*count = 0;
	return -ENOMEM;
}

int free_replica_chain_safe(struct page *primary_page,
			    const char *level_name, int order)
{
	struct page *cur_page;
	struct page *next_page;
	struct page *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!primary_page)
		return 0;

	cur_page = xchg(&primary_page->pt_replica, NULL);
	if (!cur_page)
		return 0;

	start_page = primary_page;

	while (cur_page && cur_page != start_page &&
	       free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	/* Ensure all replica pointer updates are visible before freeing */
	smp_mb();

	for (i = 0; i < free_count; i++) {
		struct mm_struct *owner_mm = pages_to_free[i]->pt_owner_mm;
		int nid = page_to_nid(pages_to_free[i]);
		bool from_cache = PageMitosisFromCache(pages_to_free[i]);

		if (strcmp(level_name, "pte") == 0) {
			pagetable_dtor(page_ptdesc(pages_to_free[i]));
			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);
		} else if (strcmp(level_name, "pmd") == 0) {
			pagetable_dtor(page_ptdesc(pages_to_free[i]));
			if (owner_mm)
				mm_dec_nr_pmds(owner_mm);
		} else if (strcmp(level_name, "pud") == 0) {
			if (owner_mm)
				mm_dec_nr_puds(owner_mm);
		}

		pages_to_free[i]->pt_owner_mm = NULL;

		if (order == 0 && from_cache) {
			ClearPageMitosisFromCache(pages_to_free[i]);
			pages_to_free[i]->pt_replica = NULL;
			if (mitosis_cache_push(pages_to_free[i], nid, 0))
				continue;
		}

		ClearPageMitosisFromCache(pages_to_free[i]);
		__free_pages(pages_to_free[i], order);
	}

	return free_count;
}

void pgtable_repl_alloc_pte(struct mm_struct *mm, unsigned long pfn)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (!mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base_page->pt_replica))
		return;

	src_addr = page_address(base_page);
	ret = alloc_pte_replicas(base_page, mm, pages, &count);
	if (ret != 0 || count < 2)
		return;

	for (i = 1; i < count; i++) {
		memcpy(page_address(pages[i]), src_addr, PAGE_SIZE);
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
	}

	/* Recheck: replication may have been disabled concurrently */
	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_ptes(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	if (unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_ptes(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}
}
EXPORT_SYMBOL(pgtable_repl_alloc_pte);

void pgtable_repl_alloc_pmd(struct mm_struct *mm, unsigned long pfn)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (!mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base_page->pt_replica))
		return;

	src_addr = page_address(base_page);
	ret = alloc_pmd_replicas(base_page, mm, pages, &count);
	if (ret != 0 || count < 2)
		return;

	for (i = 1; i < count; i++) {
		memcpy(page_address(pages[i]), src_addr, PAGE_SIZE);
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
	}

	/* Recheck: replication may have been disabled concurrently */
	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_pmds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	if (unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_pmds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}
}
EXPORT_SYMBOL(pgtable_repl_alloc_pmd);

void pgtable_repl_alloc_pud(struct mm_struct *mm, unsigned long pfn)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (!mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base_page->pt_replica))
		return;

	src_addr = page_address(base_page);
	ret = alloc_pud_replicas(base_page, mm, pages, &count);
	if (ret != 0 || count < 2)
		return;

	for (i = 1; i < count; i++) {
		memcpy(page_address(pages[i]), src_addr, PAGE_SIZE);
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
	}

	/* Recheck: replication may have been disabled concurrently */
	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled))) {
		for (i = 1; i < count; i++) {
			mm_dec_nr_puds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	if (unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			mm_dec_nr_puds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}
}
EXPORT_SYMBOL(pgtable_repl_alloc_pud);

void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (!pgtable_l5_enabled() || !mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base_page->pt_replica))
		return;

	src_addr = page_address(base_page);
	ret = alloc_p4d_replicas(base_page, mm, pages, &count);
	if (ret != 0 || count < 2)
		return;

	for (i = 1; i < count; i++) {
		memcpy(page_address(pages[i]), src_addr, PAGE_SIZE);
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
	}

	/* Recheck: replication may have been disabled concurrently */
	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled))) {
		for (i = 1; i < count; i++) {
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	if (unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}
}
EXPORT_SYMBOL(pgtable_repl_alloc_p4d);

void pgtable_repl_release_pte(struct mm_struct *mm, unsigned long pfn)
{
	struct page *page;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);

	cur_page = xchg(&page->pt_replica, NULL);
	if (!cur_page)
		return;

	start_page = page;

	while (cur_page && cur_page != start_page &&
	       free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	for (i = 0; i < free_count; i++) {
		struct mm_struct *owner_mm = pages_to_free[i]->pt_owner_mm;
		int nid = page_to_nid(pages_to_free[i]);
		bool from_cache = PageMitosisFromCache(pages_to_free[i]);

		pagetable_dtor(page_ptdesc(pages_to_free[i]));
		if (owner_mm)
			mm_dec_nr_ptes(owner_mm);
		pages_to_free[i]->pt_owner_mm = NULL;

		if (from_cache) {
			ClearPageMitosisFromCache(pages_to_free[i]);
			pages_to_free[i]->pt_replica = NULL;
			if (mitosis_cache_push(pages_to_free[i], nid, 0))
				continue;
		}

		ClearPageMitosisFromCache(pages_to_free[i]);
		__free_page(pages_to_free[i]);
	}
}
EXPORT_SYMBOL(pgtable_repl_release_pte);

void pgtable_repl_release_pmd(struct mm_struct *mm, unsigned long pfn)
{
	struct page *page;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);

	cur_page = xchg(&page->pt_replica, NULL);
	if (!cur_page)
		return;

	start_page = page;

	while (cur_page && cur_page != start_page &&
	       free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	for (i = 0; i < free_count; i++) {
		struct mm_struct *owner_mm = pages_to_free[i]->pt_owner_mm;
		int nid = page_to_nid(pages_to_free[i]);
		bool from_cache = PageMitosisFromCache(pages_to_free[i]);

		pagetable_dtor(page_ptdesc(pages_to_free[i]));
		if (owner_mm)
			mm_dec_nr_pmds(owner_mm);
		pages_to_free[i]->pt_owner_mm = NULL;

		if (from_cache) {
			ClearPageMitosisFromCache(pages_to_free[i]);
			pages_to_free[i]->pt_replica = NULL;
			if (mitosis_cache_push(pages_to_free[i], nid, 0))
				continue;
		}

		ClearPageMitosisFromCache(pages_to_free[i]);
		__free_page(pages_to_free[i]);
	}
}
EXPORT_SYMBOL(pgtable_repl_release_pmd);

void pgtable_repl_release_pud(struct mm_struct *mm, unsigned long pfn)
{
	struct page *page;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);

	cur_page = xchg(&page->pt_replica, NULL);
	if (!cur_page)
		return;

	start_page = page;

	while (cur_page && cur_page != start_page &&
	       free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	for (i = 0; i < free_count; i++) {
		struct mm_struct *owner_mm = pages_to_free[i]->pt_owner_mm;
		int nid = page_to_nid(pages_to_free[i]);
		bool from_cache = PageMitosisFromCache(pages_to_free[i]);

		if (owner_mm)
			mm_dec_nr_puds(owner_mm);
		pages_to_free[i]->pt_owner_mm = NULL;

		if (from_cache) {
			ClearPageMitosisFromCache(pages_to_free[i]);
			pages_to_free[i]->pt_replica = NULL;
			if (mitosis_cache_push(pages_to_free[i], nid, 0))
				continue;
		}

		ClearPageMitosisFromCache(pages_to_free[i]);
		__free_page(pages_to_free[i]);
	}
}
EXPORT_SYMBOL(pgtable_repl_release_pud);

void pgtable_repl_release_p4d(struct mm_struct *mm, unsigned long pfn)
{
	struct page *page;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pgtable_l5_enabled() || !pfn_valid(pfn))
		return;

	page = pfn_to_page(pfn);

	cur_page = xchg(&page->pt_replica, NULL);
	if (!cur_page)
		return;

	start_page = page;

	while (cur_page && cur_page != start_page &&
	       free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	for (i = 0; i < free_count; i++) {
		int nid = page_to_nid(pages_to_free[i]);
		bool from_cache = PageMitosisFromCache(pages_to_free[i]);

		pages_to_free[i]->pt_owner_mm = NULL;

		if (from_cache) {
			ClearPageMitosisFromCache(pages_to_free[i]);
			pages_to_free[i]->pt_replica = NULL;
			if (mitosis_cache_push(pages_to_free[i], nid, 0))
				continue;
		}

		ClearPageMitosisFromCache(pages_to_free[i]);
		__free_page(pages_to_free[i]);
	}
}
EXPORT_SYMBOL(pgtable_repl_release_p4d);
