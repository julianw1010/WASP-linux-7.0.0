#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <asm/pgtable.h>
#include <asm/mitosis.h>
#include <asm/tlbflush.h>
#include <linux/jump_label.h>
#include <linux/mitosis_stats.h>

DEFINE_STATIC_KEY_FALSE(mitosis_repl_ever_enabled);

void mitosis_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;

	mitosis_stats_pt_write(ptep, MITOSIS_CACHE_PTE);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!ptep ||
	    !virt_addr_valid(ptep))
		goto native_only;

	pte_page = virt_to_page(ptep);

	if (!pte_page || !pfn_valid(page_to_pfn(pte_page)) ||
	    !pte_page->pt_replica) {
		native_set_pte(ptep, pteval);
		return;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = pte_page;
	cur_page = pte_page;

	do {
		pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);

		WRITE_ONCE(*replica_entry, pteval);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	native_set_pte(ptep, pteval);
}

void mitosis_set_pmd(pmd_t *pmdp, pmd_t pmdval)
{
	struct page *parent_page;
	struct page *child_base_page = NULL;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	unsigned long entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;

	mitosis_stats_pt_write(pmdp, MITOSIS_CACHE_PMD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pmdp ||
	    !virt_addr_valid(pmdp))
		goto native_only;

	parent_page = virt_to_page(pmdp);

	if (!parent_page || !pfn_valid(page_to_pfn(parent_page)) ||
	    !parent_page->pt_replica) {
		native_set_pmd(pmdp, pmdval);
		return;
	}

	entry_val = pmd_val(pmdval);

	has_child = pmd_present(pmdval) &&
		    !pmd_trans_huge(pmdval) &&
		    !pmd_leaf(pmdval) &&
		    entry_val != 0;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;

		child_base_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		BUG_ON(!child_base_page->pt_replica);
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child) {
			struct page *node_local_child = mitosis_get_replica_for_node(child_base_page, node);

			BUG_ON(!node_local_child);

			node_val = __pa(page_address(node_local_child)) | (entry_val & ~pfn_mask);
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, __pmd(node_val));

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	native_set_pmd(pmdp, pmdval);
}

void mitosis_set_pud(pud_t *pudp, pud_t pudval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;

	mitosis_stats_pt_write(pudp, MITOSIS_CACHE_PUD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pudp ||
	    !virt_addr_valid(pudp))
		goto native_only;

	parent_page = virt_to_page(pudp);

	if (!parent_page || !pfn_valid(page_to_pfn(parent_page)) ||
	    !parent_page->pt_replica) {
		native_set_pud(pudp, pudval);
		return;
	}

	entry_val = pud_val(pudval);
	has_child = pud_present(pudval) && !pud_trans_huge(pudval) && entry_val != 0;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;

		child_base_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		BUG_ON(!child_base_page->pt_replica);
	}

	offset = ((unsigned long)pudp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child) {
			struct page *node_local_child = mitosis_get_replica_for_node(child_base_page, node);

			BUG_ON(!node_local_child);

			node_val = __pa(page_address(node_local_child)) | (entry_val & ~pfn_mask);
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, node_val);

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	native_set_pud(pudp, pudval);
}

void mitosis_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool pti_mirror = !pgtable_l5_enabled() && mitosis_pti_active();

	mitosis_stats_pt_write(p4dp, pgtable_l5_enabled() ?
			       MITOSIS_CACHE_P4D : MITOSIS_CACHE_PGD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!p4dp ||
	    !virt_addr_valid(p4dp))
		goto native_only;

	parent_page = virt_to_page(p4dp);

	if (!parent_page || !pfn_valid(page_to_pfn(parent_page))) {
		native_set_p4d(p4dp, p4dval);
		return;
	}

	if (!parent_page->pt_replica) {
		native_set_p4d(p4dp, p4dval);

		if (pti_mirror) {
			pgd_t *user_entry = mitosis_get_user_pgd_entry((pgd_t *)p4dp);

			if (user_entry)
				WRITE_ONCE(*user_entry, __pgd(p4d_val(p4dval)));
		}
		return;
	}

	entry_val = p4d_val(p4dval);
	has_child = p4d_present(p4dval) && entry_val != 0;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;

		child_base_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		BUG_ON(!child_base_page->pt_replica);
	}

	offset = ((unsigned long)p4dp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child) {
			struct page *node_local_child = mitosis_get_replica_for_node(child_base_page, node);

			BUG_ON(!node_local_child);

			node_val = __pa(page_address(node_local_child)) | (entry_val & ~pfn_mask);
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, node_val);

		if (pti_mirror) {
			pgd_t *user_entry = mitosis_get_user_pgd_entry((pgd_t *)replica_entry);

			if (user_entry)
				WRITE_ONCE(*user_entry, __pgd(node_val));
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	native_set_p4d(p4dp, p4dval);
}

void mitosis_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool pti_mirror = mitosis_pti_active();

	mitosis_stats_pt_write(pgdp, MITOSIS_CACHE_PGD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pgdp ||
	    !virt_addr_valid(pgdp))
		goto native_only;

	parent_page = virt_to_page(pgdp);

	if (!parent_page || !pfn_valid(page_to_pfn(parent_page))) {
		native_set_pgd(pgdp, pgdval);
		return;
	}

	if (!parent_page->pt_replica) {
		native_set_pgd(pgdp, pgdval);

		if (pti_mirror) {
			pgd_t *user_entry = mitosis_get_user_pgd_entry(pgdp);

			if (user_entry)
				WRITE_ONCE(*user_entry, __pgd(pgd_val(pgdval)));
		}
		return;
	}

	entry_val = pgd_val(pgdval);
	has_child = pgd_present(pgdval) && entry_val != 0;

	if (has_child) {
		unsigned long child_phys = entry_val & pfn_mask;

		child_base_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		BUG_ON(!child_base_page->pt_replica);
	}

	offset = ((unsigned long)pgdp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child) {
			struct page *node_local_child = mitosis_get_replica_for_node(child_base_page, node);

			BUG_ON(!node_local_child);

			node_val = __pa(page_address(node_local_child)) | (entry_val & ~pfn_mask);
		} else {
			node_val = entry_val;
		}

		WRITE_ONCE(*replica_entry, node_val);

		if (pti_mirror) {
			pgd_t *user_entry = mitosis_get_user_pgd_entry((pgd_t *)replica_entry);

			if (user_entry)
				WRITE_ONCE(*user_entry, __pgd(node_val));
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	native_set_pgd(pgdp, pgdval);
}

pte_t mitosis_get_pte(pte_t *ptep)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pte_t val;

	if (!ptep)
		return __pte(0);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return *ptep;

	if (!virt_addr_valid(ptep))
		return *ptep;

	page = virt_to_page(ptep);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return *ptep;

	val = __pte(0);
	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pte_t entry_val =
			*(pte_t *)(page_address(cur_page) + offset);

		if (cur_page == start_page) {
			val = entry_val;
			if (!(pte_val(entry_val) & _PAGE_PRESENT))
				break;
		} else if (pte_val(entry_val) & _PAGE_PRESENT) {
			val = __pte(pte_val(val) |
				    (pte_val(entry_val) & PTE_FLAGS_MASK));
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}

pte_t mitosis_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pte_t val;

	if (!ptep)
		return __pte(0);

	mitosis_stats_pt_write(ptep, MITOSIS_CACHE_PTE);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return native_ptep_get_and_clear(ptep);

	if (!virt_addr_valid(ptep))
		return native_ptep_get_and_clear(ptep);

	page = virt_to_page(ptep);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return native_ptep_get_and_clear(ptep);

	val = __pte(0);
	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pte_t old_val = native_ptep_get_and_clear(
			(pte_t *)(page_address(cur_page) + offset));

		if (cur_page == start_page)
			val = old_val;
		else
			val = __pte(pte_val(val) |
				    (pte_val(old_val) & PTE_FLAGS_MASK));

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}

static void repl_wrprotect_pte_one(pte_t *ptep)
{
	pte_t old_pte, new_pte;

	old_pte = READ_ONCE(*ptep);
	do {
		new_pte = pte_wrprotect(old_pte);
	} while (!try_cmpxchg((long *)&ptep->pte, (long *)&old_pte, *(long *)&new_pte));
}

static void repl_wrprotect_pmd_one(pmd_t *pmdp)
{
	pmd_t old_pmd, new_pmd;

	old_pmd = READ_ONCE(*pmdp);
	do {
		new_pmd = pmd_wrprotect(old_pmd);
	} while (!try_cmpxchg((long *)pmdp, (long *)&old_pmd, *(long *)&new_pmd));
}

static void repl_set_wrprotect_pte_entry(pte_t *ptep)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!ptep || !virt_addr_valid(ptep))
		goto native_only;

	page = virt_to_page(ptep);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		repl_wrprotect_pte_one(ptep);
		return;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pte_t *replica_entry =
			(pte_t *)(page_address(cur_page) + offset);

		repl_wrprotect_pte_one(replica_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	repl_wrprotect_pte_one(ptep);
}

static void repl_set_wrprotect_pmd_entry(pmd_t *pmdp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pmdp || !virt_addr_valid(pmdp))
		goto native_only;

	page = virt_to_page(pmdp);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		repl_wrprotect_pmd_one(pmdp);
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pmd_t *replica_entry =
			(pmd_t *)(page_address(cur_page) + offset);

		repl_wrprotect_pmd_one(replica_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	repl_wrprotect_pmd_one(pmdp);
}

void mitosis_ptep_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pte_t *ptep)
{
	mitosis_stats_pt_write(ptep, MITOSIS_CACHE_PTE);
	repl_set_wrprotect_pte_entry(ptep);
}

int mitosis_ptep_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pte_t *ptep)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	int young = 0;

	mitosis_stats_pt_write(ptep, MITOSIS_CACHE_PTE);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!ptep || !virt_addr_valid(ptep))
		goto native_only;

	page = virt_to_page(ptep);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		if (pte_young(*ptep))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)&ptep->pte);
		return young;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pte_t *replica_entry =
			(pte_t *)(page_address(cur_page) + offset);

		if (pte_young(*replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)&replica_entry->pte))
				young = 1;
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return young;

native_only:
	if (pte_young(*ptep))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)&ptep->pte);
	return young;
}

pmd_t mitosis_pmdp_huge_get_and_clear(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmd_t val;

	if (!pmdp)
		return __pmd(0);

	mitosis_stats_pt_write(pmdp, MITOSIS_CACHE_PMD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return native_pmdp_get_and_clear(pmdp);

	if (!virt_addr_valid(pmdp))
		return native_pmdp_get_and_clear(pmdp);

	page = virt_to_page(pmdp);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return native_pmdp_get_and_clear(pmdp);

	val = __pmd(0);
	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pmd_t old_val = native_pmdp_get_and_clear(
			(pmd_t *)(page_address(cur_page) + offset));

		if (cur_page == start_page)
			val = old_val;
		else
			val = __pmd(pmd_val(val) |
				    (pmd_val(old_val) & PTE_FLAGS_MASK));

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}

void mitosis_pmdp_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pmd_t *pmdp)
{
	mitosis_stats_pt_write(pmdp, MITOSIS_CACHE_PMD);
	repl_set_wrprotect_pmd_entry(pmdp);
}

void mitosis_free_pte_replicas(struct mm_struct *mm, struct page *page)
{
	mitosis_free_replica_chain(page, MITOSIS_CACHE_PTE, 0, NULL);

}

pmd_t mitosis_pmdp_establish(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmdval_t val;

	mitosis_stats_pt_write(pmdp, MITOSIS_CACHE_PMD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pmdp || !virt_addr_valid(pmdp))
		goto native_only;

	pmd_page = virt_to_page(pmdp);

	if (!pmd_page || !pfn_valid(page_to_pfn(pmd_page)))
		goto native_only;

	if (!pmd_page->pt_replica) {
		if (IS_ENABLED(CONFIG_SMP))
			return xchg(pmdp, pmd);
		else {
			pmd_t old = *pmdp;

			WRITE_ONCE(*pmdp, pmd);
			return old;
		}
	}

	val = 0;
	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = pmd_page;
	cur_page = pmd_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);
		pmd_t old_entry;

		if (IS_ENABLED(CONFIG_SMP)) {
			old_entry = xchg(replica_entry, pmd);
		} else {
			old_entry = *replica_entry;
			WRITE_ONCE(*replica_entry, pmd);
		}

		if (cur_page == start_page)
			val = pmd_val(old_entry);
		val |= pmd_flags(old_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return __pmd(val);

native_only:
	if (IS_ENABLED(CONFIG_SMP)) {
		return xchg(pmdp, pmd);
	} else {
		pmd_t old = *pmdp;

		WRITE_ONCE(*pmdp, pmd);
		return old;
	}
}

int mitosis_pmdp_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pmd_t *pmdp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	int young = 0;

	mitosis_stats_pt_write(pmdp, MITOSIS_CACHE_PMD);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!pmdp || !virt_addr_valid(pmdp))
		goto native_only;

	page = virt_to_page(pmdp);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		if (pmd_young(*pmdp))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)pmdp);
		return young;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pmd_t *replica_entry =
			(pmd_t *)(page_address(cur_page) + offset);

		if (pmd_young(*replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)replica_entry))
				young = 1;
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return young;

native_only:
	if (pmd_young(*pmdp))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)pmdp);
	return young;
}

pmd_t mitosis_get_pmd(pmd_t *pmdp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmd_t val;

	if (!pmdp)
		return __pmd(0);

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return *pmdp;

	if (!virt_addr_valid(pmdp))
		return *pmdp;

	page = virt_to_page(pmdp);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return *pmdp;

	val = __pmd(0);
	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		pmd_t entry_val =
			*(pmd_t *)(page_address(cur_page) + offset);

		if (cur_page == start_page) {
			val = entry_val;
			if (!(pmd_val(entry_val) & _PAGE_PRESENT))
				break;
		} else if (pmd_val(entry_val) & _PAGE_PRESENT) {
			val = __pmd(pmd_val(val) |
				    (pmd_val(entry_val) & PTE_FLAGS_MASK));
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}
