#include <linux/mm.h>
#include <asm/pgtable_repl.h>

int sysctl_mitosis_verify_enabled;

void mitosis_verify_chain_integrity(struct page *primary, struct mm_struct *mm,
				    int level)
{
	struct page *replica;
	nodemask_t seen_nodes;
	int count;
	int primary_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !primary)
		return;

	replica = READ_ONCE(primary->pt_replica);
	if (!replica)
		return;

	primary_nid = page_to_nid(primary);
	nodes_clear(seen_nodes);
	node_set(primary_nid, seen_nodes);
	count = 1;

	if (mm && smp_load_acquire(&mm->repl_pgd_enabled))
		BUG_ON(primary->pt_owner_mm != mm);

	while (replica != primary) {
		int replica_nid;

		BUG_ON(!replica);

		replica_nid = page_to_nid(replica);

		BUG_ON(node_isset(replica_nid, seen_nodes));
		node_set(replica_nid, seen_nodes);
		count++;

		BUG_ON(count > NUMA_NODE_COUNT);

		if (mm && smp_load_acquire(&mm->repl_pgd_enabled)) {
			BUG_ON(!node_isset(replica_nid, mm->repl_pgd_nodes));
			BUG_ON(replica->pt_owner_mm != mm);
		}

		replica = READ_ONCE(replica->pt_replica);
	}

	if (mm && smp_load_acquire(&mm->repl_pgd_enabled)) {
		int expected_count = nodes_weight(mm->repl_pgd_nodes);

		BUG_ON(count != expected_count);
		BUG_ON(!nodes_subset(mm->repl_pgd_nodes, seen_nodes));
	}
}


static void verify_page_full(struct page *primary_page,
			     struct mm_struct *mm, int level)
{
	struct page *replica;
	unsigned long *primary_entries;
	int num_entries;
	int idx;

	if (!primary_page || !READ_ONCE(primary_page->pt_replica))
		return;

	primary_entries = (unsigned long *)page_address(primary_page);
	num_entries = PAGE_SIZE / sizeof(unsigned long);

	if (level == MITOSIS_CACHE_PGD)
		num_entries = KERNEL_PGD_BOUNDARY;

	for (idx = 0; idx < num_entries; idx++) {
		unsigned long primary_val = READ_ONCE(primary_entries[idx]);
		bool primary_present = !!(primary_val & _PAGE_PRESENT);
		bool is_leaf;

		if (!primary_present && primary_val == 0)
			continue;

		if (level == MITOSIS_CACHE_PTE)
			is_leaf = true;
		else
			is_leaf = primary_present && (primary_val & _PAGE_PSE);

		replica = READ_ONCE(primary_page->pt_replica);
		while (replica && replica != primary_page) {
			unsigned long *replica_entries =
				(unsigned long *)page_address(replica);
			unsigned long replica_val = READ_ONCE(replica_entries[idx]);
			bool replica_present = !!(replica_val & _PAGE_PRESENT);
			int replica_nid = page_to_nid(replica);

			BUG_ON(primary_present && !replica_present);
			BUG_ON(!primary_present && replica_present);

			if (!primary_present) {
				BUG_ON(primary_val != replica_val);
				goto next_replica;
			}

			if (is_leaf) {
				unsigned long primary_pfn = primary_val & PTE_PFN_MASK;
				unsigned long replica_pfn = replica_val & PTE_PFN_MASK;

				BUG_ON(primary_pfn != replica_pfn);
			} else {
				unsigned long primary_child_phys = primary_val & PTE_PFN_MASK;
				unsigned long replica_child_phys = replica_val & PTE_PFN_MASK;

				if (replica_child_phys &&
				    pfn_valid(replica_child_phys >> PAGE_SHIFT)) {
					int child_nid = page_to_nid(pfn_to_page(
						replica_child_phys >> PAGE_SHIFT));

					BUG_ON(child_nid != replica_nid);
				}

				if (primary_child_phys &&
				    pfn_valid(primary_child_phys >> PAGE_SHIFT)) {
					struct page *child_page = pfn_to_page(
						primary_child_phys >> PAGE_SHIFT);

					if (READ_ONCE(child_page->pt_replica)) {
						struct page *expected_page =
							get_replica_for_node(
								child_page, replica_nid);

						if (expected_page) {
							unsigned long expected_phys =
								__pa(page_address(
									expected_page));

							BUG_ON(replica_child_phys != expected_phys);
						}
					} else {
						BUG_ON(replica_child_phys != primary_child_phys);
					}
				}
			}

			BUG_ON((primary_val & _PAGE_RW) != (replica_val & _PAGE_RW));

next_replica:
			replica = READ_ONCE(replica->pt_replica);
		}
	}
}

void mitosis_verify_tree_consistency(struct mm_struct *mm)
{
	pgd_t *pgd;
	struct page *pgd_page;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;
	int node;
	unsigned long child_phys;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	pgd = mm->pgd;
	pgd_page = virt_to_page(pgd);

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *node_pgd;
		struct page *expected_page;

		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		node_pgd = mm->pgd_replicas[node];

		BUG_ON(!node_pgd);

		expected_page = get_replica_for_node(pgd_page, node);
		BUG_ON(!expected_page || page_address(expected_page) != node_pgd);
		BUG_ON(page_to_nid(virt_to_page(node_pgd)) != node);
	}

	if (READ_ONCE(pgd_page->pt_replica))
		verify_page_full(pgd_page, mm, MITOSIS_CACHE_PGD);

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgd_ent = READ_ONCE(pgd[pgd_idx]);
		p4d_t *p4d_base;

		if (pgd_none(pgd_ent) || !pgd_present(pgd_ent))
			continue;

		if (pgtable_l5_enabled()) {
			child_phys = pgd_val(pgd_ent) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (READ_ONCE(child_page->pt_replica))
					verify_page_full(child_page, mm,
							 MITOSIS_CACHE_P4D);
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4d_ent = READ_ONCE(p4d_base[p4d_idx]);
			pud_t *pud_base;

			if (p4d_none(p4d_ent) || !p4d_present(p4d_ent))
				continue;

			child_phys = p4d_val(p4d_ent) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (READ_ONCE(child_page->pt_replica))
					verify_page_full(child_page, mm,
							 MITOSIS_CACHE_PUD);
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pud_ent = READ_ONCE(pud_base[pud_idx]);
				pmd_t *pmd_base;

				if (pud_none(pud_ent) || !pud_present(pud_ent) ||
				    pud_trans_huge(pud_ent))
					continue;

				child_phys = pud_val(pud_ent) & PTE_PFN_MASK;
				if (child_phys) {
					struct page *child_page =
						pfn_to_page(child_phys >> PAGE_SHIFT);

					if (READ_ONCE(child_page->pt_replica))
						verify_page_full(child_page, mm,
								 MITOSIS_CACHE_PMD);
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmd_ent = READ_ONCE(pmd_base[pmd_idx]);

					if (pmd_none(pmd_ent) ||
					    !pmd_present(pmd_ent) ||
					    pmd_trans_huge(pmd_ent) ||
					    pmd_leaf(pmd_ent))
						continue;

					child_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
					if (child_phys) {
						struct page *child_page = pfn_to_page(
							child_phys >> PAGE_SHIFT);

						if (READ_ONCE(child_page->pt_replica))
							verify_page_full(
								child_page, mm,
								MITOSIS_CACHE_PTE);
					}
				}
			}
		}
	}
}

void mitosis_verify_after_fork(struct mm_struct *child, struct mm_struct *parent)
{
	struct page *child_pgd_page, *parent_pgd_page;
	struct page *child_replica, *parent_replica;
	pgd_t *pgd;
	int node;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;
	unsigned long child_phys;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !child || !parent ||
	    !smp_load_acquire(&child->repl_pgd_enabled))
		return;

	BUG_ON(child->pgd == parent->pgd);

	child_pgd_page = virt_to_page(child->pgd);
	parent_pgd_page = virt_to_page(parent->pgd);

	BUG_ON(child_pgd_page == parent_pgd_page);

	if (smp_load_acquire(&parent->repl_pgd_enabled)) {
		for (node = 0; node < NUMA_NODE_COUNT; node++) {
			pgd_t *child_pgd_replica;
			pgd_t *parent_pgd_replica;

			if (!node_isset(node, child->repl_pgd_nodes))
				continue;

			child_pgd_replica = child->pgd_replicas[node];
			parent_pgd_replica = parent->pgd_replicas[node];

			if (!child_pgd_replica || !parent_pgd_replica)
				continue;

			BUG_ON(child_pgd_replica == parent_pgd_replica);
		}

		child_replica = child_pgd_page;
		do {
			parent_replica = parent_pgd_page;
			do {
				BUG_ON(child_replica == parent_replica);
				parent_replica = READ_ONCE(parent_replica->pt_replica);
			} while (parent_replica && parent_replica != parent_pgd_page);

			child_replica = READ_ONCE(child_replica->pt_replica);
		} while (child_replica && child_replica != child_pgd_page);
	}

	pgd = child->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgd_ent = READ_ONCE(pgd[pgd_idx]);
		p4d_t *p4d_base;

		if (pgd_none(pgd_ent) || !pgd_present(pgd_ent))
			continue;

		if (pgtable_l5_enabled()) {
			child_phys = pgd_val(pgd_ent) & PTE_PFN_MASK;
			if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(child_page->pt_owner_mm == parent);
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4d_ent = READ_ONCE(p4d_base[p4d_idx]);
			pud_t *pud_base;

			if (p4d_none(p4d_ent) || !p4d_present(p4d_ent))
				continue;

			child_phys = p4d_val(p4d_ent) & PTE_PFN_MASK;
			if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(child_page->pt_owner_mm == parent);
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pud_ent = READ_ONCE(pud_base[pud_idx]);
				pmd_t *pmd_base;

				if (pud_none(pud_ent) || !pud_present(pud_ent) ||
				    pud_trans_huge(pud_ent))
					continue;

				child_phys = pud_val(pud_ent) & PTE_PFN_MASK;
				if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
					struct page *child_page =
						pfn_to_page(child_phys >> PAGE_SHIFT);

					BUG_ON(child_page->pt_owner_mm == parent);
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmd_ent = READ_ONCE(pmd_base[pmd_idx]);

					if (pmd_none(pmd_ent) ||
					    !pmd_present(pmd_ent) ||
					    pmd_trans_huge(pmd_ent) ||
					    pmd_leaf(pmd_ent))
						continue;

					child_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
					if (child_phys &&
					    pfn_valid(child_phys >> PAGE_SHIFT)) {
						struct page *child_page = pfn_to_page(
							child_phys >> PAGE_SHIFT);

						BUG_ON(child_page->pt_owner_mm == parent);
					}
				}
			}
		}
	}
}

void mitosis_verify_pti_consistency(struct mm_struct *mm)
{
	struct page *pgd_page, *replica;
	int pgd_idx;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    !mitosis_pti_active())
		return;

	pgd_page = virt_to_page(mm->pgd);
	replica = pgd_page;

	do {
		pgd_t *kernel_pgd = (pgd_t *)page_address(replica);
		int replica_nid = page_to_nid(replica);

		for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
			pgd_t *user_entry =
				mitosis_get_user_pgd_entry(&kernel_pgd[pgd_idx]);
			pgd_t kernel_val, user_val;
			unsigned long kernel_pfn, user_pfn;
			bool kernel_present, user_present;

			if (!user_entry)
				continue;

			kernel_val = READ_ONCE(kernel_pgd[pgd_idx]);
			user_val = READ_ONCE(*user_entry);

			kernel_present = pgd_present(kernel_val) && !pgd_none(kernel_val);
			user_present = pgd_present(user_val) && !pgd_none(user_val);

			BUG_ON(user_present && !kernel_present);

			if (!kernel_present || !user_present)
				continue;

			kernel_pfn = pgd_val(kernel_val) & PTE_PFN_MASK;
			user_pfn = pgd_val(user_val) & PTE_PFN_MASK;

			BUG_ON(kernel_pfn != user_pfn);

			if (kernel_pfn && pfn_valid(kernel_pfn >> PAGE_SHIFT)) {
				int child_nid = page_to_nid(
					pfn_to_page(kernel_pfn >> PAGE_SHIFT));

				BUG_ON(child_nid != replica_nid);
			}
		}

		replica = READ_ONCE(replica->pt_replica);
	} while (replica && replica != pgd_page);
}

void mitosis_verify_cache_pop(struct page *page, int node)
{
	unsigned long *entries;
	int idx;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !page)
		return;

	BUG_ON(page_to_nid(page) != node);
	BUG_ON(page->pt_owner_mm);
	BUG_ON(page->pt_replica);
	BUG_ON(!PageMitosisFromCache(page));

	entries = (unsigned long *)page_address(page);
	for (idx = 0; idx < PAGE_SIZE / sizeof(unsigned long); idx++)
		BUG_ON(entries[idx] != 0);
}

void mitosis_verify_mm_coherence(struct mm_struct *mm)
{
	int nid;
	int primary_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm ||
	    !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	BUG_ON(nodes_empty(mm->repl_pgd_nodes));
	BUG_ON(nodes_weight(mm->repl_pgd_nodes) < 2);
	BUG_ON(!mm->original_pgd);
	BUG_ON(mm->pgd != mm->original_pgd);

	primary_nid = page_to_nid(virt_to_page(mm->original_pgd));

	BUG_ON(!node_isset(primary_nid, mm->repl_pgd_nodes));
	BUG_ON(mm->pgd_replicas[primary_nid] != mm->original_pgd);

	for (nid = 0; nid < NUMA_NODE_COUNT; nid++) {
		if (!node_isset(nid, mm->repl_pgd_nodes))
			continue;

		BUG_ON(!mm->pgd_replicas[nid]);
		BUG_ON(page_to_nid(virt_to_page(mm->pgd_replicas[nid])) != nid);
	}

	for (nid = 0; nid < NUMA_NODE_COUNT; nid++)
		BUG_ON(!node_isset(nid, mm->repl_pgd_nodes) && mm->pgd_replicas[nid]);

	for (nid = 0; nid < NUMA_NODE_COUNT; nid++) {
		int target = READ_ONCE(mm->repl_steering[nid]);

		if (target == -1)
			continue;

		BUG_ON(target < 0 || target >= NUMA_NODE_COUNT);
		BUG_ON(!node_isset(target, mm->repl_pgd_nodes));
		BUG_ON(!mm->pgd_replicas[target]);
	}
}

void mitosis_verify_after_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	replica = READ_ONCE(pte_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	while (replica != pte_page) {
		pte_t *replica_entry = (pte_t *)(page_address(replica) + offset);
		pte_t replica_pte = READ_ONCE(*replica_entry);

		if (pte_present(pteval)) {
			BUG_ON(!pte_present(replica_pte));
			BUG_ON(pte_pfn(pteval) != pte_pfn(replica_pte));
			BUG_ON(pte_write(pteval) != pte_write(replica_pte));
		} else {
			BUG_ON(pte_present(replica_pte));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_set_pmd(pmd_t *pmdp, pmd_t pmdval)
{
	struct page *parent_page, *replica;
	unsigned long offset;
	unsigned long entry_val;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	parent_page = virt_to_page(pmdp);
	replica = READ_ONCE(parent_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	entry_val = pmd_val(pmdval);

	while (replica != parent_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(replica);

		if (pmd_present(pmdval) && (pmd_trans_huge(pmdval) || pmd_leaf(pmdval))) {
			BUG_ON(!pmd_present(replica_pmd) ||
			       (!pmd_trans_huge(replica_pmd) && !pmd_leaf(replica_pmd)));
			BUG_ON(pmd_pfn(pmdval) != pmd_pfn(replica_pmd));
			BUG_ON(pmd_write(pmdval) != pmd_write(replica_pmd));
		} else if (pmd_present(pmdval) && entry_val != 0) {
			unsigned long child_phys = pmd_val(replica_pmd) & PTE_PFN_MASK;

			BUG_ON(!pmd_present(replica_pmd));
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(page_to_nid(child_page) != replica_nid);
			}
		} else {
			BUG_ON(pmd_present(replica_pmd));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_set_pud(pud_t *pudp, pud_t pudval)
{
	struct page *parent_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pudp || !virt_addr_valid(pudp))
		return;

	parent_page = virt_to_page(pudp);
	replica = READ_ONCE(parent_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pudp) & ~PAGE_MASK;

	while (replica != parent_page) {
		pud_t *replica_entry = (pud_t *)(page_address(replica) + offset);
		pud_t replica_pud = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(replica);

		if (pud_present(pudval) && !pud_trans_huge(pudval)) {
			unsigned long child_phys;

			BUG_ON(!pud_present(replica_pud));
			child_phys = pud_val(replica_pud) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(page_to_nid(child_page) != replica_nid);
			}
		} else if (!pud_present(pudval)) {
			BUG_ON(pud_present(replica_pud));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	struct page *parent_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !p4dp || !virt_addr_valid(p4dp))
		return;

	parent_page = virt_to_page(p4dp);
	replica = READ_ONCE(parent_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)p4dp) & ~PAGE_MASK;

	while (replica != parent_page) {
		p4d_t *replica_entry = (p4d_t *)(page_address(replica) + offset);
		p4d_t replica_p4d = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(replica);

		if (p4d_present(p4dval)) {
			unsigned long child_phys;

			BUG_ON(!p4d_present(replica_p4d));
			child_phys = p4d_val(replica_p4d) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(page_to_nid(child_page) != replica_nid);
			}
		} else {
			BUG_ON(p4d_present(replica_p4d));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	struct page *parent_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pgdp || !virt_addr_valid(pgdp))
		return;

	parent_page = virt_to_page(pgdp);
	replica = READ_ONCE(parent_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pgdp) & ~PAGE_MASK;

	while (replica != parent_page) {
		pgd_t *replica_entry = (pgd_t *)(page_address(replica) + offset);
		pgd_t replica_pgd = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(replica);

		if (pgd_present(pgdval)) {
			unsigned long child_phys;

			BUG_ON(!pgd_present(replica_pgd));
			child_phys = pgd_val(replica_pgd) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(page_to_nid(child_page) != replica_nid);
			}
		} else {
			BUG_ON(pgd_present(replica_pgd));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_thp_split(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *pmd_page, *pmd_replica;
	struct page *primary_pte_page;
	pte_t *primary_ptes;
	unsigned long pmd_offset;
	unsigned long pte_phys;
	pmd_t pmd_ent;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	if (!READ_ONCE(pmd_page->pt_replica))
		return;

	pmd_offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	pmd_ent = READ_ONCE(*pmdp);

	BUG_ON(pmd_trans_huge(pmd_ent) || pmd_leaf(pmd_ent));
	BUG_ON(!pmd_present(pmd_ent) || pmd_val(pmd_ent) == 0);

	pte_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
	primary_pte_page = pfn_to_page(pte_phys >> PAGE_SHIFT);
	primary_ptes = (pte_t *)page_address(primary_pte_page);

	pmd_replica = READ_ONCE(pmd_page->pt_replica);
	while (pmd_replica && pmd_replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(pmd_replica) + pmd_offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(pmd_replica);
		unsigned long child_phys;
		struct page *child_pte_page;
		pte_t *replica_ptes;
		int pte_idx;

		BUG_ON(pmd_trans_huge(replica_pmd) || pmd_leaf(replica_pmd));
		BUG_ON(!pmd_present(replica_pmd));

		child_phys = pmd_val(replica_pmd) & PTE_PFN_MASK;
		child_pte_page = pfn_to_page(child_phys >> PAGE_SHIFT);

		BUG_ON(page_to_nid(child_pte_page) != replica_nid);

		replica_ptes = (pte_t *)page_address(child_pte_page);

		for (pte_idx = 0; pte_idx < PTRS_PER_PTE; pte_idx++) {
			pte_t primary_pte = READ_ONCE(primary_ptes[pte_idx]);
			pte_t replica_pte = READ_ONCE(replica_ptes[pte_idx]);

			BUG_ON(pte_present(primary_pte) != pte_present(replica_pte));
			BUG_ON(pte_present(primary_pte) &&
			       pte_pfn(primary_pte) != pte_pfn(replica_pte));
		}

		pmd_replica = READ_ONCE(pmd_replica->pt_replica);
	}
	mitosis_verify_chain_integrity(pmd_page, mm, MITOSIS_CACHE_PMD);
	mitosis_verify_chain_integrity(primary_pte_page, mm, MITOSIS_CACHE_PTE);
}

void mitosis_verify_after_ptep_get_and_clear(pte_t *ptep)
{
	struct page *pte_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	replica = READ_ONCE(pte_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	BUG_ON(pte_val(READ_ONCE(*ptep)) != 0);

	while (replica != pte_page) {
		pte_t *replica_entry = (pte_t *)(page_address(replica) + offset);

		BUG_ON(pte_val(READ_ONCE(*replica_entry)) != 0);

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_ptep_set_wrprotect(pte_t *ptep)
{
	struct page *pte_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	replica = READ_ONCE(pte_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	BUG_ON(pte_present(READ_ONCE(*ptep)) && pte_write(READ_ONCE(*ptep)));

	while (replica != pte_page) {
		pte_t *replica_entry = (pte_t *)(page_address(replica) + offset);
		pte_t replica_pte = READ_ONCE(*replica_entry);

		BUG_ON(pte_present(replica_pte) && pte_write(replica_pte));

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_pmdp_set_wrprotect(pmd_t *pmdp)
{
	struct page *pmd_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	replica = READ_ONCE(pmd_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	BUG_ON(pmd_present(READ_ONCE(*pmdp)) && pmd_write(READ_ONCE(*pmdp)));

	while (replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);

		BUG_ON(pmd_present(replica_pmd) && pmd_write(replica_pmd));

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_pmdp_huge_get_and_clear(pmd_t *pmdp)
{
	struct page *pmd_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	replica = READ_ONCE(pmd_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	BUG_ON(pmd_val(READ_ONCE(*pmdp)) != 0);

	while (replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);

		BUG_ON(pmd_val(READ_ONCE(*replica_entry)) != 0);

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_pmdp_establish(pmd_t *pmdp, pmd_t newpmd)
{
	struct page *pmd_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	replica = READ_ONCE(pmd_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	while (replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);
		int replica_nid = page_to_nid(replica);

		if (pmd_present(newpmd) && (pmd_trans_huge(newpmd) || pmd_leaf(newpmd))) {
			BUG_ON(!pmd_present(replica_pmd) ||
			       (!pmd_trans_huge(replica_pmd) && !pmd_leaf(replica_pmd)));
			BUG_ON(pmd_pfn(newpmd) != pmd_pfn(replica_pmd));
		} else if (pmd_present(newpmd)) {
			unsigned long child_phys;

			BUG_ON(!pmd_present(replica_pmd));
			child_phys = pmd_val(replica_pmd) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(page_to_nid(child_page) != replica_nid);
			}
		} else {
			BUG_ON(pmd_present(replica_pmd));
		}

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_deposit(struct mm_struct *mm, pmd_t *pmdp,
				  pgtable_t pgtable)
{
	int pmd_nid, pte_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) || !pgtable ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pmd_nid = page_to_nid(virt_to_page(pmdp));
	pte_nid = page_to_nid(pgtable);

	BUG_ON(pmd_nid != pte_nid);
}

void mitosis_verify_after_withdraw(struct mm_struct *mm, pmd_t *pmdp,
				   pgtable_t pgtable)
{
	int pmd_nid, pte_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	BUG_ON(!pgtable);

	pmd_nid = page_to_nid(virt_to_page(pmdp));
	pte_nid = page_to_nid(pgtable);

	BUG_ON(pmd_nid != pte_nid);
}

void mitosis_verify_get_pte(pte_t *ptep, pte_t result)
{
}

void mitosis_verify_get_pmd(pmd_t *pmdp, pmd_t result)
{
}

void mitosis_verify_after_ptep_clear_young(pte_t *ptep)
{
	struct page *pte_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	replica = READ_ONCE(pte_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	BUG_ON(pte_young(READ_ONCE(*ptep)));

	while (replica != pte_page) {
		pte_t *replica_entry = (pte_t *)(page_address(replica) + offset);

		BUG_ON(pte_young(READ_ONCE(*replica_entry)));

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_pmdp_clear_young(pmd_t *pmdp)
{
	struct page *pmd_page, *replica;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	replica = READ_ONCE(pmd_page->pt_replica);
	if (!replica)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	BUG_ON(pmd_present(READ_ONCE(*pmdp)) && pmd_young(READ_ONCE(*pmdp)));

	while (replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);

		BUG_ON(pmd_present(replica_pmd) && pmd_trans_huge(replica_pmd) &&
		       pmd_young(replica_pmd));

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
}

void mitosis_verify_after_repl_alloc(struct mm_struct *mm, unsigned long pfn,
				     int level)
{
	struct page *primary, *replica;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !pfn_valid(pfn) ||
	    !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	primary = pfn_to_page(pfn);
	replica = READ_ONCE(primary->pt_replica);
	if (!replica)
		return;

	mitosis_verify_chain_integrity(primary, mm, level);

	replica = READ_ONCE(primary->pt_replica);

	while (replica != primary) {
		unsigned long *src_entries = (unsigned long *)page_address(primary);
		unsigned long *dst_entries = (unsigned long *)page_address(replica);
		int idx;

		for (idx = 0; idx < PAGE_SIZE / sizeof(unsigned long); idx++) {
			unsigned long src_val = READ_ONCE(src_entries[idx]);
			unsigned long dst_val = READ_ONCE(dst_entries[idx]);

			if (src_val & _PAGE_PRESENT)
				BUG_ON((dst_val & ~_PAGE_ACCESSED) !=
				       (src_val & ~_PAGE_ACCESSED));
		}

		replica = READ_ONCE(replica->pt_replica);
		BUG_ON(!replica);
	}
}

void mitosis_verify_after_thp_collapse(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *pmd_page, *replica, *huge_page;
	unsigned long offset;
	pmd_t pmd_ent;
	unsigned long huge_pfn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pmd_page = virt_to_page(pmdp);
	if (!READ_ONCE(pmd_page->pt_replica))
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	pmd_ent = READ_ONCE(*pmdp);

	BUG_ON(!pmd_present(pmd_ent) || !pmd_trans_huge(pmd_ent));

	huge_pfn = pmd_pfn(pmd_ent);
	huge_page = pfn_to_page(huge_pfn);

	BUG_ON(!PageCompound(huge_page));
	BUG_ON(compound_order(compound_head(huge_page)) != HPAGE_PMD_ORDER);

	replica = READ_ONCE(pmd_page->pt_replica);
	while (replica != pmd_page) {
		pmd_t *replica_entry = (pmd_t *)(page_address(replica) + offset);
		pmd_t replica_pmd = READ_ONCE(*replica_entry);

		BUG_ON(!pmd_present(replica_pmd) || !pmd_trans_huge(replica_pmd));
		BUG_ON(pmd_pfn(replica_pmd) != huge_pfn);

		replica = READ_ONCE(replica->pt_replica);
		if (!replica)
			break;
	}
	mitosis_verify_chain_integrity(pmd_page, mm, MITOSIS_CACHE_PMD);
}

static void verify_enable_walk_tree(struct mm_struct *mm, const char *caller)
{
	pgd_t *pgd = mm->pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgd_ent = READ_ONCE(pgd[pgd_idx]);
		p4d_t *p4d_base;

		if (pgd_none(pgd_ent) || !pgd_present(pgd_ent))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long child_phys = pgd_val(pgd_ent) & PTE_PFN_MASK;

			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(!READ_ONCE(child_page->pt_replica));
				mitosis_verify_chain_integrity(child_page, mm,
							       MITOSIS_CACHE_P4D);
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4d_ent = READ_ONCE(p4d_base[p4d_idx]);
			pud_t *pud_base;
			unsigned long child_phys;

			if (p4d_none(p4d_ent) || !p4d_present(p4d_ent))
				continue;

			child_phys = p4d_val(p4d_ent) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				BUG_ON(!READ_ONCE(child_page->pt_replica));
				mitosis_verify_chain_integrity(child_page, mm,
							       MITOSIS_CACHE_PUD);
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pud_ent = READ_ONCE(pud_base[pud_idx]);
				pmd_t *pmd_base;

				if (pud_none(pud_ent) || !pud_present(pud_ent) ||
				    pud_trans_huge(pud_ent))
					continue;

				child_phys = pud_val(pud_ent) & PTE_PFN_MASK;
				if (child_phys) {
					struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

					BUG_ON(!READ_ONCE(child_page->pt_replica));
					mitosis_verify_chain_integrity(child_page, mm,
								       MITOSIS_CACHE_PMD);
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmd_ent = READ_ONCE(pmd_base[pmd_idx]);

					if (pmd_none(pmd_ent) || !pmd_present(pmd_ent) ||
					    pmd_trans_huge(pmd_ent) || pmd_leaf(pmd_ent))
						continue;

					child_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
					if (child_phys) {
						struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

						BUG_ON(!READ_ONCE(child_page->pt_replica));
						mitosis_verify_chain_integrity(child_page, mm,
									       MITOSIS_CACHE_PTE);
					}
				}
			}
		}
	}
}

void mitosis_verify_after_enable(struct mm_struct *mm)
{
	struct page *pgd_page;
	int node;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pgd_page = virt_to_page(mm->pgd);
	BUG_ON(!READ_ONCE(pgd_page->pt_replica));

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		BUG_ON(!mm->pgd_replicas[node]);
		BUG_ON(page_to_nid(virt_to_page(mm->pgd_replicas[node])) != node);
	}

	verify_enable_walk_tree(mm, "enable");

	mitosis_verify_tree_consistency(mm);

	mitosis_verify_pti_consistency(mm);

	mitosis_verify_mm_coherence(mm);
}

static void verify_disable_walk_tree(struct mm_struct *mm)
{
	pgd_t *pgd = mm->pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgd_ent = READ_ONCE(pgd[pgd_idx]);
		p4d_t *p4d_base;

		if (pgd_none(pgd_ent) || !pgd_present(pgd_ent))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long child_phys = pgd_val(pgd_ent) & PTE_PFN_MASK;

			if (child_phys)
				BUG_ON(READ_ONCE(pfn_to_page(child_phys >> PAGE_SHIFT)->pt_replica));
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4d_ent = READ_ONCE(p4d_base[p4d_idx]);
			pud_t *pud_base;
			unsigned long child_phys;

			if (p4d_none(p4d_ent) || !p4d_present(p4d_ent))
				continue;

			child_phys = p4d_val(p4d_ent) & PTE_PFN_MASK;
			if (child_phys)
				BUG_ON(READ_ONCE(pfn_to_page(child_phys >> PAGE_SHIFT)->pt_replica));

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pud_ent = READ_ONCE(pud_base[pud_idx]);
				pmd_t *pmd_base;

				if (pud_none(pud_ent) || !pud_present(pud_ent) ||
				    pud_trans_huge(pud_ent))
					continue;

				child_phys = pud_val(pud_ent) & PTE_PFN_MASK;
				if (child_phys)
					BUG_ON(READ_ONCE(pfn_to_page(child_phys >> PAGE_SHIFT)->pt_replica));

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmd_ent = READ_ONCE(pmd_base[pmd_idx]);

					if (pmd_none(pmd_ent) || !pmd_present(pmd_ent) ||
					    pmd_trans_huge(pmd_ent) || pmd_leaf(pmd_ent))
						continue;

					child_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
					if (child_phys)
						BUG_ON(READ_ONCE(pfn_to_page(child_phys >> PAGE_SHIFT)->pt_replica));
				}
			}
		}
	}
}

void mitosis_verify_after_disable(struct mm_struct *mm)
{
	struct page *pgd_page;
	int nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !mm)
		return;

	BUG_ON(mm->repl_pgd_enabled);
	BUG_ON(!nodes_empty(mm->repl_pgd_nodes));
	BUG_ON(mm->original_pgd);

	pgd_page = virt_to_page(mm->pgd);
	BUG_ON(READ_ONCE(pgd_page->pt_replica));

	for (nid = 0; nid < NUMA_NODE_COUNT; nid++)
		BUG_ON(mm->pgd_replicas[nid]);

	verify_disable_walk_tree(mm);
}

void mitosis_verify_after_cr3_switch(struct mm_struct *mm)
{
	unsigned long cr3_phys;
	struct page *cr3_page;
	int cr3_nid, local_nid, target_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	cr3_phys = __read_cr3() & PAGE_MASK;
	if (!cr3_phys || !pfn_valid(cr3_phys >> PAGE_SHIFT))
		return;

	cr3_page = pfn_to_page(cr3_phys >> PAGE_SHIFT);
	cr3_nid = page_to_nid(cr3_page);
	local_nid = numa_node_id();

	target_nid = READ_ONCE(mm->repl_steering[local_nid]);
	if (target_nid < 0 || target_nid >= NUMA_NODE_COUNT)
		target_nid = local_nid;

	if (!mm->pgd_replicas[target_nid]) {
		if (cr3_phys != __pa(mm->pgd))
			return;
	} else {
		BUG_ON(cr3_phys != __pa(mm->pgd_replicas[target_nid]));
	}

	BUG_ON(cr3_nid != target_nid);
}

void mitosis_verify_after_free_replicas(struct page *primary, int level)
{
	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !primary)
		return;

	BUG_ON(READ_ONCE(primary->pt_replica));
}

void mitosis_verify_fault_locality(struct mm_struct *mm, unsigned long address)
{
	int local_nid;
	unsigned long cr3_phys;
	pgd_t *pgd, pgd_ent;
	p4d_t *p4d_ptr, p4d_ent;
	pud_t *pud_ptr, pud_ent;
	pmd_t *pmd_ptr, pmd_ent;
	unsigned long child_phys;
	int child_nid;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress) ||
	    address >= TASK_SIZE)
		return;

	local_nid = numa_node_id();
	cr3_phys = __read_cr3() & PAGE_MASK;
	if (!cr3_phys || !pfn_valid(cr3_phys >> PAGE_SHIFT))
		return;

	child_nid = page_to_nid(pfn_to_page(cr3_phys >> PAGE_SHIFT));
	BUG_ON(child_nid != local_nid);

	pgd = __va(cr3_phys);
	pgd_ent = READ_ONCE(pgd[pgd_index(address)]);
	if (pgd_none(pgd_ent) || !pgd_present(pgd_ent))
		return;

	if (pgtable_l5_enabled()) {
		child_phys = pgd_val(pgd_ent) & PTE_PFN_MASK;
		if (child_phys) {
			child_nid = page_to_nid(pfn_to_page(child_phys >> PAGE_SHIFT));
			BUG_ON(child_nid != local_nid);
		}
	}

	p4d_ptr = p4d_offset(&pgd[pgd_index(address)], address);
	p4d_ent = READ_ONCE(*p4d_ptr);
	if (p4d_none(p4d_ent) || !p4d_present(p4d_ent))
		return;

	child_phys = p4d_val(p4d_ent) & PTE_PFN_MASK;
	if (child_phys) {
		child_nid = page_to_nid(pfn_to_page(child_phys >> PAGE_SHIFT));
		BUG_ON(child_nid != local_nid);
	}

	pud_ptr = pud_offset(p4d_ptr, address);
	pud_ent = READ_ONCE(*pud_ptr);
	if (pud_none(pud_ent) || !pud_present(pud_ent) || pud_trans_huge(pud_ent))
		return;

	child_phys = pud_val(pud_ent) & PTE_PFN_MASK;
	if (child_phys) {
		child_nid = page_to_nid(pfn_to_page(child_phys >> PAGE_SHIFT));
		BUG_ON(child_nid != local_nid);
	}

	pmd_ptr = pmd_offset(pud_ptr, address);
	pmd_ent = READ_ONCE(*pmd_ptr);
	if (pmd_none(pmd_ent) || !pmd_present(pmd_ent) ||
	    pmd_trans_huge(pmd_ent) || pmd_leaf(pmd_ent))
		return;

	child_phys = pmd_val(pmd_ent) & PTE_PFN_MASK;
	if (child_phys) {
		child_nid = page_to_nid(pfn_to_page(child_phys >> PAGE_SHIFT));
		BUG_ON(child_nid != local_nid);
	}
}
