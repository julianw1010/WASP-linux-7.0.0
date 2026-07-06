#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <asm/pgtable.h>
#include <asm/mitosis.h>
#include <asm/tlbflush.h>
#include <linux/jump_label.h>

DEFINE_STATIC_KEY_FALSE(mitosis_repl_ever_enabled);

void mitosis_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;

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

	if (unlikely(READ_ONCE(mitosis_verify))) {
		cur_page = start_page;
		do {
			pte_t *replica_entry = (pte_t *)(page_address(cur_page) + offset);

			if (pte_val(READ_ONCE(*replica_entry)) != pte_val(pteval)) {
				pr_emerg("MITOSIS verify: set_pte replica on node %d diverged (%lx != %lx)\n",
					 page_to_nid(cur_page),
					 (unsigned long)pte_val(READ_ONCE(*replica_entry)),
					 (unsigned long)pte_val(pteval));
				BUG();
			}
			cur_page = cur_page->pt_replica;
		} while (cur_page && cur_page != start_page);
	}
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
	bool is_huge;

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

	is_huge = pmd_present(pmdval) &&
		  (pmd_trans_huge(pmdval) || pmd_leaf(pmdval));

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

	if (unlikely(READ_ONCE(mitosis_verify)) && is_huge) {
		cur_page = start_page;
		do {
			pmd_t *replica_entry = (pmd_t *)(page_address(cur_page) + offset);

			if (pmd_val(READ_ONCE(*replica_entry)) != pmd_val(pmdval)) {
				pr_emerg("MITOSIS verify: set_pmd huge replica on node %d diverged (%lx != %lx)\n",
					 page_to_nid(cur_page),
					 (unsigned long)pmd_val(READ_ONCE(*replica_entry)),
					 (unsigned long)pmd_val(pmdval));
				BUG();
			}
			cur_page = cur_page->pt_replica;
		} while (cur_page && cur_page != start_page);
	}
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

static unsigned long repl_get_entry(void *entryp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	unsigned long val;

	if (!entryp)
		return 0;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return *(unsigned long *)entryp;

	if (!virt_addr_valid(entryp))
		return *(unsigned long *)entryp;

	page = virt_to_page(entryp);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return *(unsigned long *)entryp;

	val = 0;
	offset = ((unsigned long)entryp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long entry_val = *replica_entry;

		if (cur_page == start_page) {
			val = entry_val;
			if (!(entry_val & _PAGE_PRESENT))
				break;
		} else if (entry_val & _PAGE_PRESENT) {
			val |= entry_val & PTE_FLAGS_MASK;
		}
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}

pte_t mitosis_get_pte(pte_t *ptep)
{
	return __pte(repl_get_entry(ptep));
}

static unsigned long repl_get_and_clear_entry(void *entryp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	unsigned long val = 0;

	if (!entryp)
		return 0;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		return xchg((unsigned long *)entryp, 0);

	if (!virt_addr_valid(entryp))
		return xchg((unsigned long *)entryp, 0);

	page = virt_to_page(entryp);

	if (!page || !pfn_valid(page_to_pfn(page)) ||
	    !page->pt_replica)
		return xchg((unsigned long *)entryp, 0);

	offset = ((unsigned long)entryp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);
		unsigned long old_val = xchg(replica_entry, 0);

		if (cur_page == start_page)
			val = old_val;
		else
			val |= old_val & PTE_FLAGS_MASK;

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return val;
}

pte_t mitosis_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	return __pte(repl_get_and_clear_entry(ptep));
}

static void repl_set_wrprotect_entry(void *entryp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	unsigned long old_val, new_val;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!entryp || !virt_addr_valid(entryp))
		goto native_only;

	page = virt_to_page(entryp);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		old_val = READ_ONCE(*(unsigned long *)entryp);
		do {
			new_val = old_val & ~_PAGE_RW;
		} while (!try_cmpxchg((long *)entryp, (long *)&old_val, *(long *)&new_val));
		return;
	}

	offset = ((unsigned long)entryp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);

		old_val = READ_ONCE(*replica_entry);
		do {
			new_val = old_val & ~_PAGE_RW;
		} while (!try_cmpxchg((long *)replica_entry, (long *)&old_val, *(long *)&new_val));
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return;

native_only:
	old_val = READ_ONCE(*(unsigned long *)entryp);
	do {
		new_val = old_val & ~_PAGE_RW;
	} while (!try_cmpxchg((long *)entryp, (long *)&old_val, *(long *)&new_val));
}

void mitosis_ptep_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pte_t *ptep)
{
	repl_set_wrprotect_entry(ptep);
}

static int repl_test_and_clear_young_entry(void *entryp)
{
	struct page *page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	int young = 0;

	if (!static_branch_unlikely(&mitosis_repl_ever_enabled))
		goto native_only;

	if (!entryp || !virt_addr_valid(entryp))
		goto native_only;

	page = virt_to_page(entryp);

	if (!page || !pfn_valid(page_to_pfn(page)))
		goto native_only;

	if (!page->pt_replica) {
		if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)entryp);
		return young;
	}

	offset = ((unsigned long)entryp) & ~PAGE_MASK;
	start_page = page;
	cur_page = page;

	do {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur_page) + offset);

		if (test_bit(_PAGE_BIT_ACCESSED, replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED, replica_entry))
				young = 1;
		}

		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
	return young;

native_only:
	if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)entryp);
	return young;
}

int mitosis_ptep_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pte_t *ptep)
{
	return repl_test_and_clear_young_entry(ptep);
}

pmd_t mitosis_pmdp_huge_get_and_clear(struct mm_struct *mm, pmd_t *pmdp)
{
	return __pmd(repl_get_and_clear_entry(pmdp));
}

void mitosis_pmdp_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pmd_t *pmdp)
{
	repl_set_wrprotect_entry(pmdp);
}

void mitosis_free_pte_replicas(struct mm_struct *mm, struct page *page)
{
	mitosis_free_replica_chain(page, MITOSIS_CACHE_PTE, 0);

}

pmd_t mitosis_pmdp_establish(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur_page;
	struct page *start_page;
	unsigned long offset;
	pmdval_t val;

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
	return repl_test_and_clear_young_entry(pmdp);
}

pmd_t mitosis_get_pmd(pmd_t *pmdp)
{
	return __pmd(repl_get_entry(pmdp));
}
