#include <asm/pgtable.h>
#include <asm/pgtable_repl.h>
#include <asm/tlbflush.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

int sysctl_wasp_auto_enable = -1;
EXPORT_SYMBOL(sysctl_wasp_auto_enable);

static int __init wasp_setup(char *str)
{
	sysctl_wasp_auto_enable = 1;
	return 1;
}
__setup("wasp", wasp_setup);

static struct ptdesc *get_replica_for_node(struct ptdesc *base, int target_node)
{
	struct ptdesc *p, *start;

	if (!base)
		return NULL;
	if (page_to_nid(ptdesc_page(base)) == target_node)
		return base;
	p = READ_ONCE(base->pt_replica);
	if (!p)
		return NULL;
	start = base;
	while (p != start) {
		if (page_to_nid(ptdesc_page(p)) == target_node)
			return p;
		p = READ_ONCE(p->pt_replica);
		if (!p)
			return NULL;
	}
	return NULL;
}

void pgtable_repl_set_pte(pte_t *ptep, pte_t pteval)
{
	struct ptdesc *pte_ptdesc, *cur, *start;
	unsigned long offset;

	if (!ptep || !virt_addr_valid(ptep)) {
		native_set_pte(ptep, pteval);
		return;
	}

	pte_ptdesc = virt_to_ptdesc(ptep);
	if (!pte_ptdesc) {
		native_set_pte(ptep, pteval);
		return;
	}

	if (!READ_ONCE(pte_ptdesc->pt_replica)) {
		native_set_pte(ptep, pteval);
		return;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start = pte_ptdesc;
	native_set_pte(ptep, pteval);

	cur = READ_ONCE(pte_ptdesc->pt_replica);
	while (cur && cur != start) {
		pte_t *replica_entry = (pte_t *)(ptdesc_address(cur) + offset);
		WRITE_ONCE(*replica_entry, pte_mkold(pteval));
		cur = READ_ONCE(cur->pt_replica);
	}

	smp_wmb();
}

void pgtable_repl_set_pmd(pmd_t *pmdp, pmd_t pmdval)
{
	struct ptdesc *parent, *cur, *start, *child_base;
	unsigned long offset, entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child, child_has_replicas;

	if (!pmdp || !virt_addr_valid(pmdp)) {
		native_set_pmd(pmdp, pmdval);
		return;
	}

	parent = virt_to_ptdesc(pmdp);
	if (!parent) {
		native_set_pmd(pmdp, pmdval);
		return;
	}

	if (!READ_ONCE(parent->pt_replica)) {
		native_set_pmd(pmdp, pmdval);
		return;
	}

	entry_val = pmd_val(pmdval);
	has_child = pmd_present(pmdval) &&
		    !pmd_leaf(pmdval) &&
		    entry_val != 0;

	child_base = NULL;
	child_has_replicas = false;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);
			child_base = page_ptdesc(cp);
			child_has_replicas = (READ_ONCE(child_base->pt_replica) != NULL);
		}
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start = parent;
	cur = parent;

	do {
		pmd_t *replica_entry = (pmd_t *)(ptdesc_address(cur) + offset);
		unsigned long node_val;
		int node = page_to_nid(ptdesc_page(cur));

		if (has_child && child_has_replicas && child_base) {
			struct ptdesc *local = get_replica_for_node(child_base, node);
			if (local) {
				unsigned long local_phys = __pa(ptdesc_address(local));
				node_val = local_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, __pmd(node_val));
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
}

void pgtable_repl_set_pud(pud_t *pudp, pud_t pudval)
{
	struct ptdesc *parent, *cur, *start, *child_base;
	unsigned long offset, entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child, child_has_replicas;

	if (!pudp || !virt_addr_valid(pudp)) {
		native_set_pud(pudp, pudval);
		return;
	}

	parent = virt_to_ptdesc(pudp);
	if (!parent) {
		native_set_pud(pudp, pudval);
		return;
	}

	if (!READ_ONCE(parent->pt_replica)) {
		native_set_pud(pudp, pudval);
		return;
	}

	entry_val = pud_val(pudval);
	has_child = pud_present(pudval) && !pud_trans_huge(pudval) && entry_val != 0;

	child_base = NULL;
	child_has_replicas = false;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);
			child_base = page_ptdesc(cp);
			child_has_replicas = (READ_ONCE(child_base->pt_replica) != NULL);
		}
	}

	offset = ((unsigned long)pudp) & ~PAGE_MASK;
	start = parent;
	cur = parent;

	do {
		pud_t *replica_entry = (pud_t *)(ptdesc_address(cur) + offset);
		unsigned long node_val;
		int node = page_to_nid(ptdesc_page(cur));

		if (has_child && child_has_replicas && child_base) {
			struct ptdesc *local = get_replica_for_node(child_base, node);
			if (local) {
				unsigned long local_phys = __pa(ptdesc_address(local));
				node_val = local_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, __pud(node_val));
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
}

void pgtable_repl_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	struct ptdesc *parent, *cur, *start, *child_base;
	unsigned long offset, entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child, child_has_replicas;

	if (!p4dp || !virt_addr_valid(p4dp)) {
		native_set_p4d(p4dp, p4dval);
		return;
	}

	parent = virt_to_ptdesc(p4dp);
	if (!parent) {
		native_set_p4d(p4dp, p4dval);
		return;
	}

	if (!READ_ONCE(parent->pt_replica)) {
		native_set_p4d(p4dp, p4dval);
		return;
	}

	entry_val = p4d_val(p4dval);
	has_child = p4d_present(p4dval) && entry_val != 0;

	child_base = NULL;
	child_has_replicas = false;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);
			child_base = page_ptdesc(cp);
			child_has_replicas = (READ_ONCE(child_base->pt_replica) != NULL);
		}
	}

	offset = ((unsigned long)p4dp) & ~PAGE_MASK;
	start = parent;
	cur = parent;

	do {
		p4d_t *replica_entry = (p4d_t *)(ptdesc_address(cur) + offset);
		unsigned long node_val;
		int node = page_to_nid(ptdesc_page(cur));

		if (has_child && child_has_replicas && child_base) {
			struct ptdesc *local = get_replica_for_node(child_base, node);
			if (local) {
				unsigned long local_phys = __pa(ptdesc_address(local));
				node_val = local_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, __p4d(node_val));
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
}

void pgtable_repl_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	struct ptdesc *parent, *cur, *start, *child_base;
	unsigned long offset, entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child, child_has_replicas;

	if (!pgdp || !virt_addr_valid(pgdp)) {
		native_set_pgd(pgdp, pgdval);
		return;
	}

	parent = virt_to_ptdesc(pgdp);
	if (!parent) {
		native_set_pgd(pgdp, pgdval);
		return;
	}

	if (!READ_ONCE(parent->pt_replica)) {
		native_set_pgd(pgdp, pgdval);
		return;
	}

	entry_val = pgd_val(pgdval);
	has_child = pgd_present(pgdval) && entry_val != 0;

	child_base = NULL;
	child_has_replicas = false;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);
			child_base = page_ptdesc(cp);
			child_has_replicas = (READ_ONCE(child_base->pt_replica) != NULL);
		}
	}

	offset = ((unsigned long)pgdp) & ~PAGE_MASK;
	start = parent;
	cur = parent;

	do {
		pgd_t *replica_entry = (pgd_t *)(ptdesc_address(cur) + offset);
		unsigned long node_val;
		int node = page_to_nid(ptdesc_page(cur));

		if (has_child && child_has_replicas && child_base) {
			struct ptdesc *local = get_replica_for_node(child_base, node);
			if (local) {
				unsigned long local_phys = __pa(ptdesc_address(local));
				node_val = local_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, __pgd(node_val));
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
}

pte_t pgtable_repl_get_pte(pte_t *ptep)
{
	struct ptdesc *pte_ptdesc, *cur, *start;
	unsigned long offset;
	pteval_t val;

	if (!ptep || !virt_addr_valid(ptep))
		return *ptep;

	pte_ptdesc = virt_to_ptdesc(ptep);
	if (!pte_ptdesc)
		return *ptep;

	if (!READ_ONCE(pte_ptdesc->pt_replica))
		return *ptep;

	val = pte_val(*ptep);
	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start = pte_ptdesc;

	cur = READ_ONCE(pte_ptdesc->pt_replica);
	while (cur && cur != start) {
		pte_t *replica_pte = (pte_t *)(ptdesc_address(cur) + offset);
		val |= pte_val(*replica_pte);
		cur = READ_ONCE(cur->pt_replica);
	}

	return (pte_t){ .pte = val };
}

pte_t pgtable_repl_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	struct ptdesc *pte_ptdesc, *cur, *start;
	unsigned long offset;
	pteval_t val;

	if (!ptep || !virt_addr_valid(ptep))
		return native_ptep_get_and_clear(ptep);

	pte_ptdesc = virt_to_ptdesc(ptep);
	if (!pte_ptdesc)
		return native_ptep_get_and_clear(ptep);

	if (!READ_ONCE(pte_ptdesc->pt_replica))
		return native_ptep_get_and_clear(ptep);

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start = pte_ptdesc;
	val = pte_val(native_ptep_get_and_clear(ptep));

	cur = READ_ONCE(pte_ptdesc->pt_replica);
	while (cur && cur != start) {
		pte_t *replica_entry = (pte_t *)(ptdesc_address(cur) + offset);
		pte_t old = native_ptep_get_and_clear(replica_entry);
		val |= pte_val(old);
		cur = READ_ONCE(cur->pt_replica);
	}

	smp_wmb();
	return __pte(val);
}

void pgtable_repl_ptep_set_wrprotect(struct mm_struct *mm,
                                     unsigned long addr, pte_t *ptep)
{
	struct ptdesc *pte_ptdesc, *cur, *start;
	unsigned long offset;

	if (!ptep || !virt_addr_valid(ptep)) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);
		return;
	}

	pte_ptdesc = virt_to_ptdesc(ptep);
	if (!pte_ptdesc) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);
		return;
	}

	if (!READ_ONCE(pte_ptdesc->pt_replica)) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)&ptep->pte);
		return;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start = pte_ptdesc;
	cur = pte_ptdesc;

	do {
		pte_t *replica_entry = (pte_t *)(ptdesc_address(cur) + offset);
		clear_bit(_PAGE_BIT_RW, (unsigned long *)&replica_entry->pte);
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
}

int pgtable_repl_ptep_test_and_clear_young(struct vm_area_struct *vma,
                                           unsigned long addr, pte_t *ptep)
{
	struct ptdesc *pte_ptdesc, *cur, *start;
	unsigned long offset;
	int young = 0;

	if (!ptep || !virt_addr_valid(ptep)) {
		if (pte_young(*ptep))
			return test_and_clear_bit(_PAGE_BIT_ACCESSED,
						 (unsigned long *)&ptep->pte);
		return 0;
	}

	pte_ptdesc = virt_to_ptdesc(ptep);
	if (!pte_ptdesc) {
		if (pte_young(*ptep))
			return test_and_clear_bit(_PAGE_BIT_ACCESSED,
						 (unsigned long *)&ptep->pte);
		return 0;
	}

	if (!READ_ONCE(pte_ptdesc->pt_replica)) {
		if (pte_young(*ptep))
			return test_and_clear_bit(_PAGE_BIT_ACCESSED,
						 (unsigned long *)&ptep->pte);
		return 0;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start = pte_ptdesc;
	cur = pte_ptdesc;

	do {
		pte_t *replica_entry = (pte_t *)(ptdesc_address(cur) + offset);
		if (pte_young(*replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)&replica_entry->pte))
				young = 1;
		}
		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != start);

	smp_wmb();
	return young;
}

void pgtable_repl_alloc_pte(struct mm_struct *mm, unsigned long pfn)
{
	struct ptdesc *base, *pages[NUMA_NODE_COUNT];
	void *src;
	int count, i, base_node;
	nodemask_t nodes;

	if (!mm || !pfn_valid(pfn))
		return;

	base = page_ptdesc(pfn_to_page(pfn));
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base->pt_replica))
		return;

	nodes = mm->repl_pgd_nodes;
	if (nodes_weight(nodes) < 2)
		return;

	base_node = page_to_nid(ptdesc_page(base));
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	pages[0] = base;
	count = 1;

	for_each_node_mask(i, nodes) {
		struct page *np;
		struct ptdesc *new;

		if (i == base_node)
			continue;

		np = alloc_pages_node(i, GFP_NOWAIT | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!np)
			goto fail;

		new = page_ptdesc(np);
		if (!pagetable_pte_ctor(mm, new)) {
			__free_page(np);
			goto fail;
		}

		new->pt_replica = NULL;
		pages[count++] = new;
	}

	src = ptdesc_address(base);
	for (i = 1; i < count; i++)
		memcpy(ptdesc_address(pages[i]), src, PAGE_SIZE);

	smp_wmb();

	for (i = 0; i < count; i++)
		WRITE_ONCE(pages[i]->pt_replica, NULL);
	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

	smp_mb();
	return;

fail:
	for (i = 1; i < count; i++) {
		pagetable_dtor(pages[i]);
		__free_page(ptdesc_page(pages[i]));
	}
}

void pgtable_repl_alloc_pmd(struct mm_struct *mm, unsigned long pfn)
{
	struct ptdesc *base, *pages[NUMA_NODE_COUNT];
	void *src;
	int count, i, base_node;
	nodemask_t nodes;

	if (!mm || !pfn_valid(pfn))
		return;

	base = page_ptdesc(pfn_to_page(pfn));
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base->pt_replica))
		return;

	nodes = mm->repl_pgd_nodes;
	if (nodes_weight(nodes) < 2)
		return;

	base_node = page_to_nid(ptdesc_page(base));
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	pages[0] = base;
	count = 1;

	for_each_node_mask(i, nodes) {
		struct page *np;
		struct ptdesc *new;

		if (i == base_node)
			continue;

		np = alloc_pages_node(i, GFP_NOWAIT | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!np)
			goto fail;

		new = page_ptdesc(np);
		if (!pagetable_pmd_ctor(mm, new)) {
			__free_page(np);
			goto fail;
		}

		new->pt_replica = NULL;
		pages[count++] = new;
	}

	src = ptdesc_address(base);
	for (i = 1; i < count; i++)
		memcpy(ptdesc_address(pages[i]), src, PAGE_SIZE);

	smp_wmb();

	for (i = 0; i < count; i++)
		WRITE_ONCE(pages[i]->pt_replica, NULL);
	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

	smp_mb();
	return;

fail:
	for (i = 1; i < count; i++) {
		pagetable_dtor(pages[i]);
		__free_page(ptdesc_page(pages[i]));
	}
}

void pgtable_repl_alloc_pud(struct mm_struct *mm, unsigned long pfn)
{
	struct ptdesc *base, *pages[NUMA_NODE_COUNT];
	void *src;
	int count, i, base_node;
	nodemask_t nodes;

	if (!mm || !pfn_valid(pfn))
		return;

	base = page_ptdesc(pfn_to_page(pfn));
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base->pt_replica))
		return;

	nodes = mm->repl_pgd_nodes;
	if (nodes_weight(nodes) < 2)
		return;

	base_node = page_to_nid(ptdesc_page(base));
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	pages[0] = base;
	count = 1;

	for_each_node_mask(i, nodes) {
		struct page *np;
		struct ptdesc *new;

		if (i == base_node)
			continue;

		np = alloc_pages_node(i, GFP_NOWAIT | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!np)
			goto fail;

		new = page_ptdesc(np);
		pagetable_pud_ctor(new);
		new->pt_replica = NULL;
		pages[count++] = new;
	}

	src = ptdesc_address(base);
	for (i = 1; i < count; i++)
		memcpy(ptdesc_address(pages[i]), src, PAGE_SIZE);

	smp_wmb();

	for (i = 0; i < count; i++)
		WRITE_ONCE(pages[i]->pt_replica, NULL);
	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

	smp_mb();
	return;

fail:
	for (i = 1; i < count; i++) {
		pagetable_dtor(pages[i]);
		__free_page(ptdesc_page(pages[i]));
	}
}

void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn)
{
	struct ptdesc *base, *pages[NUMA_NODE_COUNT];
	void *src;
	int count, i, base_node;
	nodemask_t nodes;

	if (!pgtable_l5_enabled() || !mm || !pfn_valid(pfn))
		return;

	base = page_ptdesc(pfn_to_page(pfn));
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (READ_ONCE(base->pt_replica))
		return;

	nodes = mm->repl_pgd_nodes;
	if (nodes_weight(nodes) < 2)
		return;

	base_node = page_to_nid(ptdesc_page(base));
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	pages[0] = base;
	count = 1;

	for_each_node_mask(i, nodes) {
		struct page *np;
		struct ptdesc *new;

		if (i == base_node)
			continue;

		np = alloc_pages_node(i, GFP_NOWAIT | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!np)
			goto fail;

		new = page_ptdesc(np);
		pagetable_p4d_ctor(new);
		new->pt_replica = NULL;
		pages[count++] = new;
	}

	src = ptdesc_address(base);
	for (i = 1; i < count; i++)
		memcpy(ptdesc_address(pages[i]), src, PAGE_SIZE);

	smp_wmb();

	for (i = 0; i < count; i++)
		WRITE_ONCE(pages[i]->pt_replica, NULL);
	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

	smp_mb();
	return;

fail:
	for (i = 1; i < count; i++) {
		pagetable_dtor(pages[i]);
		__free_page(ptdesc_page(pages[i]));
	}
}

static int free_replica_chain(struct ptdesc *primary, int order)
{
	struct ptdesc *cur, *next, *start;
	struct ptdesc *to_free[NUMA_NODE_COUNT];
	int count = 0, i;

	if (!primary)
		return 0;

	cur = xchg(&primary->pt_replica, NULL);
	if (!cur)
		return 0;

	start = primary;
	while (cur && cur != start && count < NUMA_NODE_COUNT) {
		to_free[count++] = cur;
		next = READ_ONCE(cur->pt_replica);
		WRITE_ONCE(cur->pt_replica, NULL);
		cur = next;
	}

	smp_mb();

	for (i = 0; i < count; i++) {
		pagetable_dtor(to_free[i]);
		__free_pages(ptdesc_page(to_free[i]), order);
	}

	return count;
}

void pgtable_repl_free_pte_replicas(struct mm_struct *mm, struct page *page)
{
	struct ptdesc *pt;

	if (!page)
		return;

	pt = page_ptdesc(page);
	free_replica_chain(pt, 0);
}

void pgtable_repl_release_pte(unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;
	free_replica_chain(page_ptdesc(pfn_to_page(pfn)), 0);
}

void pgtable_repl_release_pmd(unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;
	free_replica_chain(page_ptdesc(pfn_to_page(pfn)), 0);
}

void pgtable_repl_release_pud(unsigned long pfn)
{
	if (!pfn_valid(pfn))
		return;
	free_replica_chain(page_ptdesc(pfn_to_page(pfn)), 0);
}

void pgtable_repl_release_p4d(unsigned long pfn)
{
	if (!pgtable_l5_enabled() || !pfn_valid(pfn))
		return;
	free_replica_chain(page_ptdesc(pfn_to_page(pfn)), 0);
}

static void replicate_existing_pagetables(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (phys) {
				struct ptdesc *pt = page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT));
				pgtable_repl_alloc_p4d(mm, phys >> PAGE_SHIFT);
				(void)pt;
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			pud_t *pud_base;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			{
				unsigned long phys = p4d_val(p4dval) & PTE_PFN_MASK;
				if (phys)
					pgtable_repl_alloc_pud(mm, phys >> PAGE_SHIFT);
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_leaf(pudval))
					continue;

				{
					unsigned long phys = pud_val(pudval) & PTE_PFN_MASK;
					if (phys)
						pgtable_repl_alloc_pmd(mm, phys >> PAGE_SHIFT);
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_leaf(pmdval))
						continue;

					{
						unsigned long phys = pmd_val(pmdval) & PTE_PFN_MASK;
						if (phys)
							pgtable_repl_alloc_pte(mm, phys >> PAGE_SHIFT);
					}
				}
			}
		}
	}

	smp_mb();
}

static void rewrite_replica_pointers(struct mm_struct *mm)
{
	struct ptdesc *pgd_ptdesc = virt_to_ptdesc(mm->pgd);
	int node;

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		struct ptdesc *node_pgd_ptdesc;
		pgd_t *node_pgd;
		int pgd_idx;

		node_pgd_ptdesc = get_replica_for_node(pgd_ptdesc, node);
		if (!node_pgd_ptdesc)
			continue;

		node_pgd = ptdesc_address(node_pgd_ptdesc);

		for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
			pgd_t pgdval;
			p4d_t *node_p4d_base;
			unsigned long phys;
			int p4d_idx;

			pgdval = READ_ONCE(node_pgd[pgd_idx]);
			if (pgd_none(pgdval) || !pgd_present(pgdval))
				continue;

			phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (phys) {
				struct ptdesc *child = page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT));
				if (READ_ONCE(child->pt_replica)) {
					struct ptdesc *local = get_replica_for_node(child, node);
					if (local) {
						unsigned long np = __pa(ptdesc_address(local));
						WRITE_ONCE(node_pgd[pgd_idx], __pgd(np | (pgd_val(pgdval) & ~PTE_PFN_MASK)));
					}
				}
			}

			node_p4d_base = p4d_offset(&node_pgd[pgd_idx], 0);

			for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
				p4d_t p4dval;
				pud_t *node_pud_base;
				int pud_idx;

				p4dval = READ_ONCE(node_p4d_base[p4d_idx]);
				if (p4d_none(p4dval) || !p4d_present(p4dval))
					continue;

				phys = p4d_val(p4dval) & PTE_PFN_MASK;
				if (phys) {
					struct ptdesc *child = page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT));
					if (READ_ONCE(child->pt_replica)) {
						struct ptdesc *local = get_replica_for_node(child, node);
						if (local) {
							unsigned long np = __pa(ptdesc_address(local));
							WRITE_ONCE(node_p4d_base[p4d_idx], __p4d(np | (p4d_val(p4dval) & ~PTE_PFN_MASK)));
						}
					}
				}

				node_pud_base = pud_offset(&node_p4d_base[p4d_idx], 0);

				for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
					pud_t pudval;
					pmd_t *node_pmd_base;
					int pmd_idx;

					pudval = READ_ONCE(node_pud_base[pud_idx]);
					if (pud_none(pudval) || !pud_present(pudval) || pud_leaf(pudval))
						continue;

					phys = pud_val(pudval) & PTE_PFN_MASK;
					if (phys) {
						struct ptdesc *child = page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT));
						if (READ_ONCE(child->pt_replica)) {
							struct ptdesc *local = get_replica_for_node(child, node);
							if (local) {
								unsigned long np = __pa(ptdesc_address(local));
								WRITE_ONCE(node_pud_base[pud_idx], __pud(np | (pud_val(pudval) & ~PTE_PFN_MASK)));
							}
						}
					}

					node_pmd_base = pmd_offset(&node_pud_base[pud_idx], 0);

					for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
						pmd_t pmdval;

						pmdval = READ_ONCE(node_pmd_base[pmd_idx]);
						if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_leaf(pmdval))
							continue;

						phys = pmd_val(pmdval) & PTE_PFN_MASK;
						if (phys) {
							struct ptdesc *child = page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT));
							if (READ_ONCE(child->pt_replica)) {
								struct ptdesc *local = get_replica_for_node(child, node);
								if (local) {
									unsigned long np = __pa(ptdesc_address(local));
									WRITE_ONCE(node_pmd_base[pmd_idx], __pmd(np | (pmd_val(pmdval) & ~PTE_PFN_MASK)));
								}
							}
						}
					}
				}
			}
		}
	}

	smp_mb();
}

static int alloc_pgd_replicas(struct mm_struct *mm, nodemask_t nodes,
			      struct ptdesc **pages, int *count)
{
	struct ptdesc *base;
	int base_node, i;

	base = virt_to_ptdesc(mm->pgd);
	base_node = page_to_nid(ptdesc_page(base));
	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	pages[0] = base;
	*count = 1;

	for_each_node_mask(i, nodes) {
		struct page *np;
		struct ptdesc *new;

		if (i == base_node)
			continue;

		np = alloc_pages_node(i, GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!np)
			return -ENOMEM;

		new = page_ptdesc(np);
		new->pt_replica = NULL;
		pages[(*count)++] = new;
	}

	return 0;
}

int pgtable_repl_enable(struct mm_struct *mm, nodemask_t nodes)
{
	struct ptdesc *pgd_pages[NUMA_NODE_COUNT];
	struct ptdesc *base;
	int count = 0, ret, i, node;

	if (!mm || mm == &init_mm || nodes_empty(nodes) || nodes_weight(nodes) < 2)
		return -EINVAL;

	for_each_node_mask(node, nodes) {
		if (!node_online(node))
			return -EINVAL;
	}

	mutex_lock(&mm->repl_mutex);

	if (mm->repl_pgd_enabled) {
		ret = nodes_equal(mm->repl_pgd_nodes, nodes) ? 0 : -EALREADY;
		mutex_unlock(&mm->repl_mutex);
		return ret;
	}

	mm->original_pgd = mm->pgd;
	base = virt_to_ptdesc(mm->pgd);

	ret = alloc_pgd_replicas(mm, nodes, pgd_pages, &count);
	if (ret) {
		mutex_unlock(&mm->repl_mutex);
		return ret;
	}

	for (i = 1; i < count; i++)
		memcpy(ptdesc_address(pgd_pages[i]), mm->pgd, PAGE_SIZE);

	for (i = 0; i < count; i++)
		WRITE_ONCE(pgd_pages[i]->pt_replica, NULL);
	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pgd_pages[i]->pt_replica, pgd_pages[i + 1]);
	WRITE_ONCE(pgd_pages[count - 1]->pt_replica, pgd_pages[0]);

	mm->repl_pgd_nodes = nodes;
	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));
	for (i = 0; i < count; i++) {
		int nid = page_to_nid(ptdesc_page(pgd_pages[i]));
		mm->pgd_replicas[nid] = ptdesc_address(pgd_pages[i]);
	}

	smp_store_release(&mm->repl_in_progress, true);
	smp_store_release(&mm->repl_pgd_enabled, true);
	smp_mb();

	replicate_existing_pagetables(mm);
	rewrite_replica_pointers(mm);

	smp_store_release(&mm->repl_in_progress, false);
	smp_mb();

	for (i = 0; i < NUMA_NODE_COUNT; i++)
		WRITE_ONCE(mm->repl_steering[i], i);
		
	pr_info("MITOSIS: Enabled page table replication for mm %px on %d nodes\n", mm, count);

	mutex_unlock(&mm->repl_mutex);
	return 0;
}

static void free_all_replicas(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (phys)
				free_replica_chain(page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT)), 0);
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			pud_t *pud_base;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			{
				unsigned long phys = p4d_val(p4dval) & PTE_PFN_MASK;
				if (phys)
					free_replica_chain(page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT)), 0);
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_leaf(pudval))
					continue;

				{
					unsigned long phys = pud_val(pudval) & PTE_PFN_MASK;
					if (phys)
						free_replica_chain(page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT)), 0);
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_leaf(pmdval))
						continue;

					{
						unsigned long phys = pmd_val(pmdval) & PTE_PFN_MASK;
						if (phys)
							free_replica_chain(page_ptdesc(pfn_to_page(phys >> PAGE_SHIFT)), 0);
					}
				}
			}
		}
	}

	smp_mb();
}

static void free_pgd_replicas(struct mm_struct *mm)
{
	struct ptdesc *primary;
	int node;

	primary = virt_to_ptdesc(mm->pgd);
	WRITE_ONCE(primary->pt_replica, NULL);
	smp_wmb();

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		pgd_t *replica_pgd;
		struct ptdesc *rp;

		replica_pgd = mm->pgd_replicas[node];
		if (!replica_pgd || replica_pgd == mm->pgd)
			continue;

		rp = virt_to_ptdesc(replica_pgd);
		WRITE_ONCE(rp->pt_replica, NULL);
		__free_page(ptdesc_page(rp));
		mm->pgd_replicas[node] = NULL;
	}
}

struct cr3_switch_info {
	struct mm_struct *mm;
	pgd_t *original_pgd;
	int initiating_cpu;
};

static void switch_cr3_ipi(void *info)
{
	struct cr3_switch_info *si = info;
	unsigned long original_pa, current_cr3, current_pa;

	if (current->mm != si->mm && current->active_mm != si->mm)
		return;

	original_pa = __pa(si->original_pgd);
	current_cr3 = __read_cr3();
	current_pa = current_cr3 & PAGE_MASK;

	if (current_pa != original_pa) {
		native_write_cr3(original_pa | (current_cr3 & ~PAGE_MASK));
		__flush_tlb_all();
	}
}

void pgtable_repl_disable(struct mm_struct *mm)
{
	struct cr3_switch_info si;
	unsigned long flags;

	if (!mm || mm == &init_mm)
		return;

	mutex_lock(&mm->repl_mutex);

	if (!mm->repl_pgd_enabled) {
		mutex_unlock(&mm->repl_mutex);
		return;
	}

	if (!mm->original_pgd)
		mm->original_pgd = mm->pgd;

	smp_store_release(&mm->repl_pgd_enabled, false);
	smp_mb();

	WRITE_ONCE(mm->pgd, mm->original_pgd);
	smp_mb();

	si.mm = mm;
	si.original_pgd = mm->original_pgd;
	si.initiating_cpu = smp_processor_id();

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm) {
		unsigned long cur_pa = __read_cr3() & PAGE_MASK;
		unsigned long orig_pa = __pa(mm->original_pgd);
		if (cur_pa != orig_pa) {
			native_write_cr3(orig_pa | (__read_cr3() & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
	local_irq_restore(flags);

	on_each_cpu_mask(mm_cpumask(mm), switch_cr3_ipi, &si, 1);

	smp_mb();
	synchronize_rcu();

	free_all_replicas(mm);
	free_pgd_replicas(mm);

	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));
	nodes_clear(mm->repl_pgd_nodes);
	mm->original_pgd = NULL;
	
	pr_info("MITOSIS: Disabled page table replication for mm %p\n", mm);

	mutex_unlock(&mm->repl_mutex);
}

struct steering_switch_info {
	struct mm_struct *mm;
	int initiating_cpu;
};

static void steering_cr3_ipi(void *info)
{
	struct steering_switch_info *si = info;
	struct mm_struct *mm = si->mm;
	int local_node, target_node;
	pgd_t *target_pgd;
	unsigned long current_pa, target_pa, new_cr3;

	if (current->mm != mm && current->active_mm != mm)
		return;

	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	local_node = numa_node_id();
	target_node = READ_ONCE(mm->repl_steering[local_node]);

	if (target_node < 0 || target_node >= NUMA_NODE_COUNT)
		target_node = local_node;

	target_pgd = READ_ONCE(mm->pgd_replicas[target_node]);
	if (!target_pgd)
		target_pgd = mm->pgd;

	current_pa = __read_cr3() & PAGE_MASK;
	target_pa = __pa(target_pgd);

	if (current_pa != target_pa) {
		new_cr3 = target_pa | (__read_cr3() & ~PAGE_MASK);
		native_write_cr3(new_cr3);
		__flush_tlb_all();
	}
}

void pgtable_repl_force_steering_switch(struct mm_struct *mm, nodemask_t *changed_nodes)
{
	struct steering_switch_info si;
	unsigned long flags;

	if (!mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	si.mm = mm;
	si.initiating_cpu = smp_processor_id();

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm)
		steering_cr3_ipi(&si);
	local_irq_restore(flags);

	on_each_cpu_mask(mm_cpumask(mm), steering_cr3_ipi, &si, 1);
}
