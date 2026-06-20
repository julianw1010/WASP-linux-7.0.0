#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/pgtable_repl.h>
#include <asm/io.h>
#include <asm/cacheflush.h>

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

		new_page = mitosis_cache_pop(i, 0);
		if (!new_page) {
			new_page = alloc_pages_node(i,
				GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, 0);
			BUG_ON(!new_page);
		}
		BUG_ON(page_to_nid(new_page) != i);

		BUG_ON(!pagetable_pte_ctor(mm, page_ptdesc(new_page)));

		new_page->pt_owner_mm = mm;
		mm_inc_nr_ptes(mm);

		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
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

		new_page = mitosis_cache_pop(i, 0);
		if (!new_page) {
			new_page = alloc_pages_node(i,
				GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, 0);
			BUG_ON(!new_page);
		}
		BUG_ON(page_to_nid(new_page) != i);

		BUG_ON(!pagetable_pmd_ctor(mm, page_ptdesc(new_page)));

		new_page->pt_owner_mm = mm;
		mm_inc_nr_pmds(mm);

		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
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

		new_page = mitosis_cache_pop(i, 0);
		if (!new_page) {
			new_page = alloc_pages_node(i,
				GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, 0);
			BUG_ON(!new_page);
		}
		BUG_ON(page_to_nid(new_page) != i);

		new_page->pt_owner_mm = mm;
		mm_inc_nr_puds(mm);

		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
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

		new_page = mitosis_cache_pop(i, 0);
		if (!new_page) {
			new_page = alloc_pages_node(i,
				GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, 0);
			BUG_ON(!new_page);
		}
		BUG_ON(page_to_nid(new_page) != i);

		new_page->pt_owner_mm = mm;

		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
}

int alloc_pgd_replicas(struct page *base_page, struct mm_struct *mm,
		       nodemask_t nodes,
		       struct page **pages, int *count)
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

		new_page = NULL;
		if (alloc_order == 0)
			new_page = mitosis_cache_pop(i, 0);
		if (!new_page) {
			new_page = alloc_pages_node(i,
				GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE,
				alloc_order);
			BUG_ON(!new_page);
		}
		BUG_ON(page_to_nid(new_page) != i);

		new_page->pt_owner_mm = mm;
		if (mm)
			WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
}

int mitosis_free_replica_chain(struct page *primary, int level, int order)
{
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!primary)
		return 0;

	cur_page = xchg(&primary->pt_replica, NULL);
	if (!cur_page)
		return 0;

	start_page = primary;

	while (cur_page && cur_page != start_page && free_count < NUMA_NODE_COUNT) {
		pages_to_free[free_count++] = cur_page;
		next_page = READ_ONCE(cur_page->pt_replica);
		WRITE_ONCE(cur_page->pt_replica, NULL);
		cur_page = next_page;
	}

	smp_mb();

	for (i = 0; i < free_count; i++) {
		struct page *p = pages_to_free[i];
		struct mm_struct *owner_mm = p->pt_owner_mm;

		if (level == MITOSIS_CACHE_PTE || level == MITOSIS_CACHE_PMD)
			pagetable_dtor(page_ptdesc(p));

		if (owner_mm) {
			if (level == MITOSIS_CACHE_PTE)
				mm_dec_nr_ptes(owner_mm);
			else if (level == MITOSIS_CACHE_PMD)
				mm_dec_nr_pmds(owner_mm);
			else if (level == MITOSIS_CACHE_PUD)
				mm_dec_nr_puds(owner_mm);
		}

		if (order == 0) {
			int nid = page_to_nid(p);
			bool from_cache = PageMitosisFromCache(p);

			p->pt_owner_mm = NULL;
			ClearPageMitosisFromCache(p);

			if (from_cache) {
				p->pt_replica = NULL;
				if (mitosis_cache_push(p, nid, level))
					continue;
			}
		}

		ClearPageMitosisFromCache(p);
		p->pt_owner_mm = NULL;
		__free_pages(p, order);
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

	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled)) ||
	    unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_ptes(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	mitosis_verify_after_repl_alloc(mm, pfn, MITOSIS_CACHE_PTE);
}

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

	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled)) ||
	    unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pagetable_dtor(page_ptdesc(pages[i]));
			mm_dec_nr_pmds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	mitosis_verify_after_repl_alloc(mm, pfn, MITOSIS_CACHE_PMD);
}

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

	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled)) ||
	    unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			mm_dec_nr_puds(mm);
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	mitosis_verify_after_repl_alloc(mm, pfn, MITOSIS_CACHE_PUD);
}

void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (!pgtable_l5_enabled())
		return;

	if (!mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

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

	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled)) ||
	    unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	mitosis_verify_after_repl_alloc(mm, pfn, MITOSIS_CACHE_P4D);
}

void pgtable_repl_release_pte(unsigned long pfn)
{
	struct page *primary;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	primary = pfn_to_page(pfn);

	cur_page = xchg(&primary->pt_replica, NULL);
	if (cur_page) {
		start_page = primary;

		while (cur_page && cur_page != start_page && free_count < NUMA_NODE_COUNT) {
			pages_to_free[free_count++] = cur_page;
			next_page = READ_ONCE(cur_page->pt_replica);
			WRITE_ONCE(cur_page->pt_replica, NULL);
			cur_page = next_page;
		}

		smp_mb();

		for (i = 0; i < free_count; i++) {
			struct page *p = pages_to_free[i];
			struct mm_struct *owner_mm = p->pt_owner_mm;
			int nid = page_to_nid(p);
			bool from_cache = PageMitosisFromCache(p);

			pagetable_dtor(page_ptdesc(p));

			if (owner_mm)
				mm_dec_nr_ptes(owner_mm);

			p->pt_owner_mm = NULL;
			ClearPageMitosisFromCache(p);

			if (from_cache) {
				p->pt_replica = NULL;
				if (mitosis_cache_push(p, nid, MITOSIS_CACHE_PTE))
					continue;
			}

			__free_page(p);
		}
	}

	mitosis_verify_after_free_replicas(primary, MITOSIS_CACHE_PTE);
}

void pgtable_repl_release_pmd(unsigned long pfn)
{
	struct page *primary;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	primary = pfn_to_page(pfn);

	cur_page = xchg(&primary->pt_replica, NULL);
	if (cur_page) {
		start_page = primary;

		while (cur_page && cur_page != start_page && free_count < NUMA_NODE_COUNT) {
			pages_to_free[free_count++] = cur_page;
			next_page = READ_ONCE(cur_page->pt_replica);
			WRITE_ONCE(cur_page->pt_replica, NULL);
			cur_page = next_page;
		}

		smp_mb();

		for (i = 0; i < free_count; i++) {
			struct page *p = pages_to_free[i];
			struct mm_struct *owner_mm = p->pt_owner_mm;
			int nid = page_to_nid(p);
			bool from_cache = PageMitosisFromCache(p);

			pagetable_dtor(page_ptdesc(p));

			if (owner_mm)
				mm_dec_nr_pmds(owner_mm);

			p->pt_owner_mm = NULL;
			ClearPageMitosisFromCache(p);

			if (from_cache) {
				p->pt_replica = NULL;
				if (mitosis_cache_push(p, nid, MITOSIS_CACHE_PMD))
					continue;
			}

			__free_page(p);
		}
	}

	mitosis_verify_after_free_replicas(primary, MITOSIS_CACHE_PMD);
}

void pgtable_repl_release_pud(unsigned long pfn)
{
	struct page *primary;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pfn_valid(pfn))
		return;

	primary = pfn_to_page(pfn);

	cur_page = xchg(&primary->pt_replica, NULL);
	if (cur_page) {
		start_page = primary;

		while (cur_page && cur_page != start_page && free_count < NUMA_NODE_COUNT) {
			pages_to_free[free_count++] = cur_page;
			next_page = READ_ONCE(cur_page->pt_replica);
			WRITE_ONCE(cur_page->pt_replica, NULL);
			cur_page = next_page;
		}

		smp_mb();

		for (i = 0; i < free_count; i++) {
			struct page *p = pages_to_free[i];
			struct mm_struct *owner_mm = p->pt_owner_mm;
			int nid = page_to_nid(p);
			bool from_cache = PageMitosisFromCache(p);

			if (owner_mm)
				mm_dec_nr_puds(owner_mm);

			p->pt_owner_mm = NULL;
			ClearPageMitosisFromCache(p);

			if (from_cache) {
				p->pt_replica = NULL;
				if (mitosis_cache_push(p, nid, MITOSIS_CACHE_PUD))
					continue;
			}

			__free_page(p);
		}
	}

	mitosis_verify_after_free_replicas(primary, MITOSIS_CACHE_PUD);
}

void pgtable_repl_release_p4d(unsigned long pfn)
{
	struct page *primary;
	struct page *cur_page, *next_page, *start_page;
	struct page *pages_to_free[NUMA_NODE_COUNT];
	int free_count = 0;
	int i;

	if (!pgtable_l5_enabled() || !pfn_valid(pfn))
		return;

	primary = pfn_to_page(pfn);

	cur_page = xchg(&primary->pt_replica, NULL);
	if (cur_page) {
		start_page = primary;

		while (cur_page && cur_page != start_page && free_count < NUMA_NODE_COUNT) {
			pages_to_free[free_count++] = cur_page;
			next_page = READ_ONCE(cur_page->pt_replica);
			WRITE_ONCE(cur_page->pt_replica, NULL);
			cur_page = next_page;
		}

		smp_mb();

		for (i = 0; i < free_count; i++) {
			struct page *p = pages_to_free[i];
			int nid = page_to_nid(p);
			bool from_cache = PageMitosisFromCache(p);

			p->pt_owner_mm = NULL;
			ClearPageMitosisFromCache(p);

			if (from_cache) {
				p->pt_replica = NULL;
				if (mitosis_cache_push(p, nid, MITOSIS_CACHE_P4D))
					continue;
			}

			__free_page(p);
		}
	}

	mitosis_verify_after_free_replicas(primary, MITOSIS_CACHE_P4D);
}

