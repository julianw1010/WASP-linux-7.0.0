#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
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
            BUG_ON(page_to_nid(page) != node);
            return page;
        }
    }


    page = alloc_pages_node(node,
        GFP_NOWAIT | GFP_ATOMIC | __GFP_ZERO | __GFP_THISNODE, order);

    BUG_ON(!page);
    BUG_ON(page_to_nid(page) != node);

    return page;
}

static int alloc_replicas(struct page *base_page, struct mm_struct *mm,
			  struct page **pages, int *count, int level)
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

		new_page = mitosis_alloc_replica_page(i, 0);

		if (level == MITOSIS_CACHE_PTE)
			BUG_ON(!pagetable_pte_ctor(mm, page_ptdesc(new_page)));
		else if (level == MITOSIS_CACHE_PMD)
			BUG_ON(!pagetable_pmd_ctor(mm, page_ptdesc(new_page)));

		new_page->pt_owner_mm = mm;

		if (level == MITOSIS_CACHE_PTE)
			mm_inc_nr_ptes(mm);
		else if (level == MITOSIS_CACHE_PMD)
			mm_inc_nr_pmds(mm);
		else if (level == MITOSIS_CACHE_PUD)
			mm_inc_nr_puds(mm);

		WRITE_ONCE(new_page->pt_replica, NULL);
		pages[(*count)++] = new_page;
	}

	return 0;
}

int alloc_pte_replicas(struct page *base_page, struct mm_struct *mm,
                              struct page **pages, int *count)
{
	return alloc_replicas(base_page, mm, pages, count, MITOSIS_CACHE_PTE);
}

int alloc_pmd_replicas(struct page *base_page, struct mm_struct *mm,
                              struct page **pages, int *count)
{
	return alloc_replicas(base_page, mm, pages, count, MITOSIS_CACHE_PMD);
}

int alloc_pud_replicas(struct page *base_page, struct mm_struct *mm,
                              struct page **pages, int *count)
{
	return alloc_replicas(base_page, mm, pages, count, MITOSIS_CACHE_PUD);
}

int alloc_p4d_replicas(struct page *base_page, struct mm_struct *mm,
                              struct page **pages, int *count)
{
	return alloc_replicas(base_page, mm, pages, count, MITOSIS_CACHE_P4D);
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

		new_page = mitosis_alloc_replica_page(i, alloc_order);

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

		if (order == 0 && mitosis_free_or_cache(p, level))
			continue;

		ClearPageMitosisFromCache(p);
		p->pt_owner_mm = NULL;
		__free_pages(p, order);
	}

	return free_count;
}
EXPORT_SYMBOL(mitosis_free_replica_chain);

static void pgtable_repl_alloc(struct mm_struct *mm, unsigned long pfn, int level)
{
	struct page *base_page;
	struct page *pages[NUMA_NODE_COUNT];
	void *src_addr;
	int count = 0;
	int i, ret;

	if (level == MITOSIS_CACHE_P4D && !pgtable_l5_enabled())
		return;

	if (!mm || !pfn_valid(pfn))
		return;

	base_page = pfn_to_page(pfn);

	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base_page->pt_replica))
		return;

	src_addr = page_address(base_page);
	ret = alloc_replicas(base_page, mm, pages, &count, level);
	if (ret != 0 || count < 2)
		return;

	for (i = 1; i < count; i++) {
		memcpy(page_address(pages[i]), src_addr, PAGE_SIZE);
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
	}

	if (unlikely(!smp_load_acquire(&mm->repl_pgd_enabled)) ||
	    unlikely(!link_page_replicas(pages, count))) {
		for (i = 1; i < count; i++) {
			if (level == MITOSIS_CACHE_PTE || level == MITOSIS_CACHE_PMD)
				pagetable_dtor(page_ptdesc(pages[i]));

			if (level == MITOSIS_CACHE_PTE)
				mm_dec_nr_ptes(mm);
			else if (level == MITOSIS_CACHE_PMD)
				mm_dec_nr_pmds(mm);
			else if (level == MITOSIS_CACHE_PUD)
				mm_dec_nr_puds(mm);

			pages[i]->pt_owner_mm = NULL;
			__free_page(pages[i]);
		}
		return;
	}

	mitosis_verify_after_repl_alloc(mm, pfn, level);
}

void pgtable_repl_alloc_pte(struct mm_struct *mm, unsigned long pfn)
{
	pgtable_repl_alloc(mm, pfn, MITOSIS_CACHE_PTE);
}

void pgtable_repl_alloc_pmd(struct mm_struct *mm, unsigned long pfn)
{
	pgtable_repl_alloc(mm, pfn, MITOSIS_CACHE_PMD);
}

void pgtable_repl_alloc_pud(struct mm_struct *mm, unsigned long pfn)
{
	pgtable_repl_alloc(mm, pfn, MITOSIS_CACHE_PUD);
}

void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn)
{
	pgtable_repl_alloc(mm, pfn, MITOSIS_CACHE_P4D);
}

void pgtable_repl_release_pte(struct mm_struct *mm, unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;

	mitosis_free_replica_chain(pfn_to_page(pfn), MITOSIS_CACHE_PTE, 0);

	mitosis_verify_after_free_replicas(pfn_to_page(pfn), MITOSIS_CACHE_PTE);
}

void pgtable_repl_release_pmd(struct mm_struct *mm, unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;

	mitosis_free_replica_chain(pfn_to_page(pfn), MITOSIS_CACHE_PMD, 0);

	mitosis_verify_after_free_replicas(pfn_to_page(pfn), MITOSIS_CACHE_PMD);
}

void pgtable_repl_release_pud(struct mm_struct *mm, unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;

	mitosis_free_replica_chain(pfn_to_page(pfn), MITOSIS_CACHE_PUD, 0);

	mitosis_verify_after_free_replicas(pfn_to_page(pfn), MITOSIS_CACHE_PUD);
}

void pgtable_repl_release_p4d(struct mm_struct *mm, unsigned long pfn)
{
	if (!pgtable_l5_enabled() || !pfn_valid(pfn))
		return;

	mitosis_free_replica_chain(pfn_to_page(pfn), MITOSIS_CACHE_P4D, 0);

	mitosis_verify_after_free_replicas(pfn_to_page(pfn), MITOSIS_CACHE_P4D);
}

EXPORT_SYMBOL(pgtable_repl_alloc_pte);
EXPORT_SYMBOL(pgtable_repl_alloc_pmd);
EXPORT_SYMBOL(pgtable_repl_alloc_pud);
EXPORT_SYMBOL(pgtable_repl_alloc_p4d);
EXPORT_SYMBOL(pgtable_repl_release_pte);
EXPORT_SYMBOL(pgtable_repl_release_pmd);
EXPORT_SYMBOL(pgtable_repl_release_pud);
EXPORT_SYMBOL(pgtable_repl_release_p4d);
