#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <asm/pgtable.h>
#include <asm/pgtable_repl.h>
#include <asm/tlbflush.h>

void pgtable_repl_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;

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

void pgtable_repl_set_pmd(pmd_t *pmdp, pmd_t pmdval)
{
	struct page *parent_page;
	struct page *child_base_page = NULL;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	unsigned long entry_val;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool child_has_replicas = false;

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
		struct page *child_page;

		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
			if (child_page && virt_addr_valid(page_address(child_page))) {
				child_base_page = child_page;
				child_has_replicas = (child_base_page->pt_replica != NULL);
			}
		}
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child && child_has_replicas && child_base_page) {
			struct page *node_local_child = get_replica_for_node(child_base_page, node);

			if (node_local_child) {
				unsigned long node_child_phys = __pa(page_address(node_local_child));

				node_val = node_child_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
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

void pgtable_repl_set_pud(pud_t *pudp, pud_t pudval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool child_has_replicas = false;

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
		child_has_replicas = (child_base_page->pt_replica != NULL);
	}

	offset = ((unsigned long)pudp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child && child_has_replicas) {
			struct page *node_local_child = get_replica_for_node(child_base_page, node);

			if (node_local_child) {
				unsigned long node_child_phys = __pa(page_address(node_local_child));

				node_val = node_child_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
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

void pgtable_repl_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool child_has_replicas = false;
	bool pti_mirror = !pgtable_l5_enabled() && mitosis_pti_active();

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
		child_has_replicas = (child_base_page->pt_replica != NULL);
	}

	offset = ((unsigned long)p4dp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child && child_has_replicas) {
			struct page *node_local_child = get_replica_for_node(child_base_page, node);

			if (node_local_child) {
				unsigned long node_child_phys = __pa(page_address(node_local_child));

				node_val = node_child_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
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

void pgtable_repl_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	struct page *parent_page;
	struct page *cur_page;
	struct page *start_page;
	struct page *child_base_page = NULL;
	unsigned long entry_val;
	unsigned long offset;
	const unsigned long pfn_mask = PTE_PFN_MASK;
	bool has_child;
	bool child_has_replicas = false;
	bool pti_mirror = mitosis_pti_active();

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
		child_has_replicas = (child_base_page->pt_replica != NULL);
	}

	offset = ((unsigned long)pgdp) & ~PAGE_MASK;
	start_page = parent_page;
	cur_page = parent_page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long node_val;
		int node = page_to_nid(cur_page);

		if (has_child && child_has_replicas) {
			struct page *node_local_child = get_replica_for_node(child_base_page, node);

			if (node_local_child) {
				unsigned long node_child_phys = __pa(page_address(node_local_child));

				node_val = node_child_phys | (entry_val & ~pfn_mask);
			} else {
				node_val = entry_val;
			}
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

pte_t pgtable_repl_get_pte(pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pteval_t val;

	if (!ptep)
		return __pte(0);

	if (!virt_addr_valid(ptep))
		return __pte(pte_val(*ptep));

	pte_page = virt_to_page(ptep);

	if (!pte_page || !pfn_valid(page_to_pfn(pte_page)) ||
	    !pte_page->pt_replica)
		return __pte(pte_val(*ptep));

	val = 0;
	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = pte_page;
	cur_page = pte_page;

	do {
		pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);

		val |= pte_val(*replica_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);

	{
		pte_t ret = (pte_t){ .pte = val };
		return ret;
	}
}

pte_t pgtable_repl_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pteval_t val = 0;

	if (!ptep)
		return __pte(0);

	if (!virt_addr_valid(ptep))
		return native_ptep_get_and_clear(ptep);

	pte_page = virt_to_page(ptep);

	if (!pte_page || !pfn_valid(page_to_pfn(pte_page)) ||
	    !pte_page->pt_replica)
		return native_ptep_get_and_clear(ptep);

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = pte_page;
	cur_page = pte_page;

	do {
		pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);
		pte_t old_entry = native_ptep_get_and_clear(replica_entry);

		val |= pte_val(old_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return __pte(val);
}

void pgtable_repl_ptep_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pte_t old_pte, new_pte;

	if (!ptep ||
	    !virt_addr_valid(ptep))
		goto native_only;

	pte_page = virt_to_page(ptep);

	if (!pte_page || !pfn_valid(page_to_pfn(pte_page)))
		goto native_only;

	if (!pte_page->pt_replica) {
		old_pte = READ_ONCE(*ptep);
		do {
			new_pte = pte_wrprotect(old_pte);
		} while (!try_cmpxchg((long *)&ptep->pte, (long *)&old_pte, *(long *)&new_pte));
		return;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = pte_page;
	cur_page = pte_page;

	do {
		pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);

		old_pte = READ_ONCE(*replica_entry);
		do {
			new_pte = pte_wrprotect(old_pte);
		} while (!try_cmpxchg((long *)&replica_entry->pte, (long *)&old_pte, *(long *)&new_pte));
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	old_pte = READ_ONCE(*ptep);
	do {
		new_pte = pte_wrprotect(old_pte);
	} while (!try_cmpxchg((long *)&ptep->pte, (long *)&old_pte, *(long *)&new_pte));
}

int pgtable_repl_ptep_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pte_t *ptep)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	int young = 0;

	if (!ptep ||
	    !virt_addr_valid(ptep))
		goto native_only;

	pte_page = virt_to_page(ptep);

	if (!pte_page || !pfn_valid(page_to_pfn(pte_page)))
		goto native_only;

	if (!pte_page->pt_replica) {
		if (pte_young(*ptep))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED, (unsigned long *)&ptep->pte);
		return young;
	}

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	start_page = pte_page;
	cur_page = pte_page;

	do {
		pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);

		if (pte_young(*replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED, (unsigned long *)&replica_entry->pte))
				young = 1;
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return young;

native_only:
	return test_and_clear_bit(_PAGE_BIT_ACCESSED, (unsigned long *)&ptep->pte);
}

pmd_t pgtable_repl_pmdp_huge_get_and_clear(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmdval_t val;

	if (!pmdp)
		return __pmd(0);

	if (!virt_addr_valid(pmdp))
		return native_pmdp_get_and_clear(pmdp);

	pmd_page = virt_to_page(pmdp);

	if (!pmd_page || !pfn_valid(page_to_pfn(pmd_page)) ||
	    !pmd_page->pt_replica)
		return native_pmdp_get_and_clear(pmdp);

	val = 0;
	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = pmd_page;
	cur_page = pmd_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);
		pmd_t old_entry = native_pmdp_get_and_clear(replica_entry);

		val |= pmd_val(old_entry);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return __pmd(val);
}

void pgtable_repl_pmdp_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmd_t old_pmd, new_pmd;

	if (!pmdp || !virt_addr_valid(pmdp))
		goto native_only;

	pmd_page = virt_to_page(pmdp);

	if (!pmd_page || !pfn_valid(page_to_pfn(pmd_page)))
		goto native_only;

	if (!pmd_page->pt_replica) {
		old_pmd = READ_ONCE(*pmdp);
		do {
			new_pmd = pmd_wrprotect(old_pmd);
		} while (!try_cmpxchg((long *)pmdp, (long *)&old_pmd, *(long *)&new_pmd));
		return;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = pmd_page;
	cur_page = pmd_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);

		if (pmd_present(*replica_entry)) {
			old_pmd = READ_ONCE(*replica_entry);
			do {
				new_pmd = pmd_wrprotect(old_pmd);
			} while (!try_cmpxchg((long *)replica_entry, (long *)&old_pmd, *(long *)&new_pmd));
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	old_pmd = READ_ONCE(*pmdp);
	do {
		new_pmd = pmd_wrprotect(old_pmd);
	} while (!try_cmpxchg((long *)pmdp, (long *)&old_pmd, *(long *)&new_pmd));
}

void pgtable_repl_free_pte_replicas(struct mm_struct *mm, struct page *page)
{
	mitosis_free_replica_chain(page, MITOSIS_CACHE_PTE, 0);

}

pmd_t pgtable_repl_pmdp_establish(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmdval_t val;

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

int pgtable_repl_pmdp_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	int ret = 0;

	if (!pmdp || !virt_addr_valid(pmdp))
		goto native_only;

	pmd_page = virt_to_page(pmdp);

	if (!pmd_page || !pfn_valid(page_to_pfn(pmd_page)))
		goto native_only;

	if (!pmd_page->pt_replica) {
		if (pmd_young(*pmdp))
			ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						 (unsigned long *)pmdp);
		return ret;
	}

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = pmd_page;
	cur_page = pmd_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);

		if (pmd_present(*replica_entry) && pmd_young(*replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED,
					       (unsigned long *)replica_entry))
				ret = 1;
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return ret;

native_only:
	if (pmd_young(*pmdp))
		ret = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					 (unsigned long *)pmdp);
	return ret;
}

pmd_t pgtable_repl_get_pmd(pmd_t *pmdp)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmdval_t val;

	if (!pmdp)
		return __pmd(0);

	if (!virt_addr_valid(pmdp))
		return *pmdp;

	pmd_page = virt_to_page(pmdp);

	if (!pmd_page || !pfn_valid(page_to_pfn(pmd_page)) ||
	    !pmd_page->pt_replica)
		return *pmdp;

	val = 0;
	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	start_page = pmd_page;
	cur_page = pmd_page;

	do {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);
		pmd_t entry_val = *replica_entry;

		if (cur_page == start_page) {
			val = pmd_val(entry_val);
			if (!pmd_present(entry_val))
				break;
		} else if (pmd_present(entry_val)) {
			val |= pmd_flags(entry_val);
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);

	{
		pmd_t ret = __pmd(val);
		return ret;
	}
}
