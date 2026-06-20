#include <linux/mm.h>
#include <asm/pgtable_repl.h>

int sysctl_mitosis_verify_enabled;

void mitosis_verify_chain_integrity(struct page *primary, struct mm_struct *mm,
				    int level)
{
	struct page *cur;
	nodemask_t seen;
	int count;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !primary)
		return;

	cur = READ_ONCE(primary->pt_replica);
	if (!cur)
		return;

	pn = page_to_nid(primary);
	nodes_clear(seen);
	node_set(pn, seen);
	count = 1;

	if (mm && smp_load_acquire(&mm->repl_pgd_enabled)) {
		if (primary->pt_owner_mm != mm) {
			pr_err("MITOSIS VERIFY chain: primary pt_owner_mm "
			       "mismatch expected=%px got=%px level=%d "
			       "pfn=0x%lx\n",
			       mm, primary->pt_owner_mm, level,
			       page_to_pfn(primary));
			BUG();
		}
	}

	while (cur != primary) {
		int rn;

		if (!cur) {
			pr_err("MITOSIS VERIFY chain: broken (NULL) at "
			       "count=%d level=%d pfn=0x%lx\n",
			       count, level, page_to_pfn(primary));
			BUG();
		}

		rn = page_to_nid(cur);

		if (node_isset(rn, seen)) {
			pr_err("MITOSIS VERIFY chain: duplicate node %d "
			       "count=%d level=%d pfn=0x%lx\n",
			       rn, count, level, page_to_pfn(primary));
			BUG();
		}
		node_set(rn, seen);
		count++;

		if (count > NUMA_NODE_COUNT) {
			pr_err("MITOSIS VERIFY chain: too long count=%d "
			       "level=%d pfn=0x%lx\n",
			       count, level, page_to_pfn(primary));
			BUG();
		}

		if (mm && smp_load_acquire(&mm->repl_pgd_enabled)) {
			if (!node_isset(rn, mm->repl_pgd_nodes)) {
				pr_err("MITOSIS VERIFY chain: node %d not in "
				       "repl_pgd_nodes level=%d pfn=0x%lx\n",
				       rn, level, page_to_pfn(primary));
				BUG();
			}

			if (cur->pt_owner_mm != mm) {
				pr_err("MITOSIS VERIFY chain: pt_owner_mm "
				       "mismatch on n%d expected=%px got=%px "
				       "level=%d pfn=0x%lx\n",
				       rn, mm, cur->pt_owner_mm, level,
				       page_to_pfn(primary));
				BUG();
			}
		}

		cur = READ_ONCE(cur->pt_replica);
	}

	if (mm && smp_load_acquire(&mm->repl_pgd_enabled)) {
		int expected = nodes_weight(mm->repl_pgd_nodes);

		if (count != expected) {
			pr_err("MITOSIS VERIFY chain: count mismatch "
			       "got=%d expected=%d level=%d pfn=0x%lx\n",
			       count, expected, level, page_to_pfn(primary));
			BUG();
		}

		if (!nodes_subset(mm->repl_pgd_nodes, seen)) {
			pr_err("MITOSIS VERIFY chain: missing nodes "
			       "have=%*pbl want=%*pbl level=%d pfn=0x%lx\n",
			       nodemask_pr_args(&seen),
			       nodemask_pr_args(&mm->repl_pgd_nodes),
			       level, page_to_pfn(primary));
			BUG();
		}
	}
}


static void verify_page_full(struct page *primary_page,
			     struct mm_struct *mm, int level)
{
	struct page *cur;
	unsigned long *primary_entries;
	int pn;
	int num_entries;
	int idx;

	if (!primary_page || !READ_ONCE(primary_page->pt_replica))
		return;

	primary_entries = (unsigned long *)page_address(primary_page);
	pn = page_to_nid(primary_page);
	num_entries = PAGE_SIZE / sizeof(unsigned long);

	if (level == MITOSIS_CACHE_PGD)
		num_entries = KERNEL_PGD_BOUNDARY;

	for (idx = 0; idx < num_entries; idx++) {
		unsigned long pval = READ_ONCE(primary_entries[idx]);
		bool p_present = !!(pval & _PAGE_PRESENT);
		bool p_leaf;

		if (!p_present && pval == 0)
			continue;

		if (level == MITOSIS_CACHE_PTE)
			p_leaf = true;
		else
			p_leaf = p_present && (pval & _PAGE_PSE);

		cur = READ_ONCE(primary_page->pt_replica);
		while (cur && cur != primary_page) {
			unsigned long *replica_entries =
				(unsigned long *)page_address(cur);
			unsigned long rval = READ_ONCE(replica_entries[idx]);
			bool r_present = !!(rval & _PAGE_PRESENT);
			int rn = page_to_nid(cur);

			if (p_present && !r_present) {
				pr_err("MITOSIS VERIFY tree: entry[%d] present "
				       "on n%d but not on n%d level=%d\n",
				       idx, pn, rn, level);
				BUG();
			}

			if (!p_present && r_present) {
				pr_err("MITOSIS VERIFY tree: entry[%d] absent "
				       "on n%d but present on n%d level=%d\n",
				       idx, pn, rn, level);
				BUG();
			}

			if (!p_present) {
				if (pval != rval) {
					pr_err("MITOSIS VERIFY tree: non-present "
					       "entry[%d] mismatch n%d=0x%lx "
					       "n%d=0x%lx level=%d\n",
					       idx, pn, pval, rn, rval, level);
					BUG();
				}
				goto next_replica;
			}

			if (p_leaf) {
				unsigned long p_pfn = pval & PTE_PFN_MASK;
				unsigned long r_pfn = rval & PTE_PFN_MASK;

				if (p_pfn != r_pfn) {
					pr_err("MITOSIS VERIFY tree: leaf pfn "
					       "mismatch entry[%d] n%d=0x%lx "
					       "n%d=0x%lx level=%d\n",
					       idx, pn, p_pfn >> PAGE_SHIFT,
					       rn, r_pfn >> PAGE_SHIFT, level);
					BUG();
				}
			} else {
				unsigned long p_child_phys = pval & PTE_PFN_MASK;
				unsigned long r_child_phys = rval & PTE_PFN_MASK;

				if (r_child_phys &&
				    pfn_valid(r_child_phys >> PAGE_SHIFT)) {
					int cn = page_to_nid(pfn_to_page(
						r_child_phys >> PAGE_SHIFT));

					if (cn != rn) {
						pr_err("MITOSIS VERIFY tree: "
						       "locality entry[%d] "
						       "replica n%d child on "
						       "n%d level=%d\n",
						       idx, rn, cn, level);
						BUG();
					}
				}

				if (p_child_phys &&
				    pfn_valid(p_child_phys >> PAGE_SHIFT)) {
					struct page *child_page = pfn_to_page(
						p_child_phys >> PAGE_SHIFT);

					if (READ_ONCE(child_page->pt_replica)) {
						struct page *expected =
							get_replica_for_node(
								child_page, rn);

						if (expected) {
							unsigned long exp_phys =
								__pa(page_address(
									expected));

							if (r_child_phys != exp_phys) {
								pr_err("MITOSIS VERIFY "
								       "tree: wrong child "
								       "entry[%d] replica "
								       "n%d got pfn 0x%lx "
								       "expected pfn "
								       "0x%lx level=%d\n",
								       idx, rn,
								       r_child_phys >> PAGE_SHIFT,
								       exp_phys >> PAGE_SHIFT,
								       level);
								BUG();
							}
						}
					} else {
						if (r_child_phys != p_child_phys) {
							pr_err("MITOSIS VERIFY "
							       "tree: unreplicated "
							       "child pfn mismatch "
							       "entry[%d] n%d=0x%lx "
							       "n%d=0x%lx level=%d\n",
							       idx, pn,
							       p_child_phys >> PAGE_SHIFT,
							       rn,
							       r_child_phys >> PAGE_SHIFT,
							       level);
							BUG();
						}
					}
				}
			}

			if ((pval & _PAGE_RW) != (rval & _PAGE_RW)) {
				pr_err("MITOSIS VERIFY tree: write flag "
				       "mismatch entry[%d] n%d=%d n%d=%d "
				       "level=%d\n",
				       idx, pn, !!(pval & _PAGE_RW),
				       rn, !!(rval & _PAGE_RW), level);
				BUG();
			}

next_replica:
			cur = READ_ONCE(cur->pt_replica);
		}
	}
}

void mitosis_verify_tree_consistency(struct mm_struct *mm)
{
	pgd_t *pgd;
	struct page *pgd_page;
	int gi, p4i, pui, pmi;
	int node;
	unsigned long cphys;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	pgd = mm->pgd;
	pgd_page = virt_to_page(pgd);

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		pgd_t *node_pgd = mm->pgd_replicas[node];
		struct page *expected;

		if (!node_pgd) {
			pr_err("MITOSIS VERIFY tree: pgd_replicas[%d] NULL\n",
			       node);
			BUG();
		}

		expected = get_replica_for_node(pgd_page, node);
		if (!expected || page_address(expected) != node_pgd) {
			pr_err("MITOSIS VERIFY tree: pgd_replicas[%d]=%px "
			       "not in pt_replica chain (expected %px)\n",
			       node, node_pgd,
			       expected ? page_address(expected) : NULL);
			BUG();
		}

		if (page_to_nid(virt_to_page(node_pgd)) != node) {
			pr_err("MITOSIS VERIFY tree: pgd_replicas[%d] on "
			       "wrong node %d\n",
			       node, page_to_nid(virt_to_page(node_pgd)));
			BUG();
		}
	}

	if (READ_ONCE(pgd_page->pt_replica))
		verify_page_full(pgd_page, mm, MITOSIS_CACHE_PGD);

	for (gi = 0; gi < KERNEL_PGD_BOUNDARY; gi++) {
		pgd_t gval = READ_ONCE(pgd[gi]);
		p4d_t *p4d_base;

		if (pgd_none(gval) || !pgd_present(gval))
			continue;

		if (pgtable_l5_enabled()) {
			cphys = pgd_val(gval) & PTE_PFN_MASK;
			if (cphys) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (READ_ONCE(p->pt_replica))
					verify_page_full(p, mm,
							 MITOSIS_CACHE_P4D);
			}
		}

		p4d_base = p4d_offset(&pgd[gi], 0);

		for (p4i = 0; p4i < PTRS_PER_P4D; p4i++) {
			p4d_t p4val = READ_ONCE(p4d_base[p4i]);
			pud_t *pud_base;

			if (p4d_none(p4val) || !p4d_present(p4val))
				continue;

			cphys = p4d_val(p4val) & PTE_PFN_MASK;
			if (cphys) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (READ_ONCE(p->pt_replica))
					verify_page_full(p, mm,
							 MITOSIS_CACHE_PUD);
			}

			pud_base = pud_offset(&p4d_base[p4i], 0);

			for (pui = 0; pui < PTRS_PER_PUD; pui++) {
				pud_t puval = READ_ONCE(pud_base[pui]);
				pmd_t *pmd_base;

				if (pud_none(puval) || !pud_present(puval) ||
				    pud_trans_huge(puval))
					continue;

				cphys = pud_val(puval) & PTE_PFN_MASK;
				if (cphys) {
					struct page *p =
						pfn_to_page(cphys >> PAGE_SHIFT);

					if (READ_ONCE(p->pt_replica))
						verify_page_full(p, mm,
								 MITOSIS_CACHE_PMD);
				}

				pmd_base = pmd_offset(&pud_base[pui], 0);

				for (pmi = 0; pmi < PTRS_PER_PMD; pmi++) {
					pmd_t pmval = READ_ONCE(pmd_base[pmi]);

					if (pmd_none(pmval) ||
					    !pmd_present(pmval) ||
					    pmd_trans_huge(pmval) ||
					    pmd_leaf(pmval))
						continue;

					cphys = pmd_val(pmval) & PTE_PFN_MASK;
					if (cphys) {
						struct page *p = pfn_to_page(
							cphys >> PAGE_SHIFT);

						if (READ_ONCE(p->pt_replica))
							verify_page_full(
								p, mm,
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
	struct page *child_cur, *parent_cur;
	pgd_t *pgd;
	int node;
	int gi, p4i, pui, pmi;
	unsigned long cphys;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !child || !parent ||
	    !smp_load_acquire(&child->repl_pgd_enabled))
		return;

	if (child->pgd == parent->pgd) {
		pr_err("MITOSIS VERIFY fork: child and parent share "
		       "primary PGD %px\n", child->pgd);
		BUG();
	}

	child_pgd_page = virt_to_page(child->pgd);
	parent_pgd_page = virt_to_page(parent->pgd);

	if (child_pgd_page == parent_pgd_page) {
		pr_err("MITOSIS VERIFY fork: child and parent PGD "
		       "same physical page pfn=0x%lx\n",
		       page_to_pfn(child_pgd_page));
		BUG();
	}

	if (smp_load_acquire(&parent->repl_pgd_enabled)) {
		for_each_node_mask(node, child->repl_pgd_nodes) {
			pgd_t *child_replica = child->pgd_replicas[node];
			pgd_t *parent_replica = parent->pgd_replicas[node];

			if (!child_replica || !parent_replica)
				continue;

			if (child_replica == parent_replica) {
				pr_err("MITOSIS VERIFY fork: pgd_replicas[%d] "
				       "shared between child and parent %px\n",
				       node, child_replica);
				BUG();
			}
		}

		child_cur = child_pgd_page;
		do {
			parent_cur = parent_pgd_page;
			do {
				if (child_cur == parent_cur) {
					pr_err("MITOSIS VERIFY fork: PGD chain "
					       "page %px (n%d) shared between "
					       "child and parent\n",
					       child_cur,
					       page_to_nid(child_cur));
					BUG();
				}
				parent_cur = READ_ONCE(parent_cur->pt_replica);
			} while (parent_cur && parent_cur != parent_pgd_page);

			child_cur = READ_ONCE(child_cur->pt_replica);
		} while (child_cur && child_cur != child_pgd_page);
	}

	pgd = child->pgd;

	for (gi = 0; gi < KERNEL_PGD_BOUNDARY; gi++) {
		pgd_t gval = READ_ONCE(pgd[gi]);
		p4d_t *p4d_base;

		if (pgd_none(gval) || !pgd_present(gval))
			continue;

		if (pgtable_l5_enabled()) {
			cphys = pgd_val(gval) & PTE_PFN_MASK;
			if (cphys && pfn_valid(cphys >> PAGE_SHIFT)) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (p->pt_owner_mm == parent) {
					pr_err("MITOSIS VERIFY fork: P4D at "
					       "PGD[%d] owned by parent\n", gi);
					BUG();
				}
			}
		}

		p4d_base = p4d_offset(&pgd[gi], 0);

		for (p4i = 0; p4i < PTRS_PER_P4D; p4i++) {
			p4d_t p4val = READ_ONCE(p4d_base[p4i]);
			pud_t *pud_base;

			if (p4d_none(p4val) || !p4d_present(p4val))
				continue;

			cphys = p4d_val(p4val) & PTE_PFN_MASK;
			if (cphys && pfn_valid(cphys >> PAGE_SHIFT)) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (p->pt_owner_mm == parent) {
					pr_err("MITOSIS VERIFY fork: PUD at "
					       "[%d][%d] owned by parent\n",
					       gi, p4i);
					BUG();
				}
			}

			pud_base = pud_offset(&p4d_base[p4i], 0);

			for (pui = 0; pui < PTRS_PER_PUD; pui++) {
				pud_t puval = READ_ONCE(pud_base[pui]);
				pmd_t *pmd_base;

				if (pud_none(puval) || !pud_present(puval) ||
				    pud_trans_huge(puval))
					continue;

				cphys = pud_val(puval) & PTE_PFN_MASK;
				if (cphys && pfn_valid(cphys >> PAGE_SHIFT)) {
					struct page *p =
						pfn_to_page(cphys >> PAGE_SHIFT);

					if (p->pt_owner_mm == parent) {
						pr_err("MITOSIS VERIFY fork: "
						       "PMD at [%d][%d][%d] "
						       "owned by parent\n",
						       gi, p4i, pui);
						BUG();
					}
				}

				pmd_base = pmd_offset(&pud_base[pui], 0);

				for (pmi = 0; pmi < PTRS_PER_PMD; pmi++) {
					pmd_t pmval = READ_ONCE(pmd_base[pmi]);

					if (pmd_none(pmval) ||
					    !pmd_present(pmval) ||
					    pmd_trans_huge(pmval) ||
					    pmd_leaf(pmval))
						continue;

					cphys = pmd_val(pmval) & PTE_PFN_MASK;
					if (cphys &&
					    pfn_valid(cphys >> PAGE_SHIFT)) {
						struct page *p = pfn_to_page(
							cphys >> PAGE_SHIFT);

						if (p->pt_owner_mm == parent) {
							pr_err("MITOSIS VERIFY "
							       "fork: PTE at "
							       "[%d][%d][%d][%d]"
							       " owned by "
							       "parent\n",
							       gi, p4i, pui,
							       pmi);
							BUG();
						}
					}
				}
			}
		}
	}
}


void mitosis_verify_pti_consistency(struct mm_struct *mm)
{
	struct page *pgd_page, *cur;
	int idx;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    !mitosis_pti_active())
		return;

	pgd_page = virt_to_page(mm->pgd);
	cur = pgd_page;

	do {
		pgd_t *kernel_pgd = (pgd_t *)page_address(cur);
		int rn = page_to_nid(cur);

		for (idx = 0; idx < KERNEL_PGD_BOUNDARY; idx++) {
			pgd_t *user_entryp =
				mitosis_get_user_pgd_entry(&kernel_pgd[idx]);
			pgd_t kval, uval;
			unsigned long k_pfn, u_pfn;
			bool k_present, u_present;

			if (!user_entryp)
				continue;

			kval = READ_ONCE(kernel_pgd[idx]);
			uval = READ_ONCE(*user_entryp);

			k_present = pgd_present(kval) && !pgd_none(kval);
			u_present = pgd_present(uval) && !pgd_none(uval);

			if (u_present && !k_present) {
				pr_err("MITOSIS VERIFY pti: user entry[%d] "
				       "present but kernel absent on n%d\n",
				       idx, rn);
				BUG();
			}

			if (!k_present || !u_present)
				continue;

			k_pfn = pgd_val(kval) & PTE_PFN_MASK;
			u_pfn = pgd_val(uval) & PTE_PFN_MASK;

			if (k_pfn != u_pfn) {
				pr_err("MITOSIS VERIFY pti: pfn mismatch "
				       "entry[%d] kernel=0x%lx user=0x%lx "
				       "on n%d\n",
				       idx, k_pfn >> PAGE_SHIFT,
				       u_pfn >> PAGE_SHIFT, rn);
				BUG();
			}

			if (k_pfn && pfn_valid(k_pfn >> PAGE_SHIFT)) {
				int cn = page_to_nid(
					pfn_to_page(k_pfn >> PAGE_SHIFT));

				if (cn != rn) {
					pr_err("MITOSIS VERIFY pti: user "
					       "entry[%d] on n%d points to "
					       "child on n%d\n", idx, rn, cn);
					BUG();
				}
			}
		}

		cur = READ_ONCE(cur->pt_replica);
	} while (cur && cur != pgd_page);
}

void mitosis_verify_cache_pop(struct page *page, int node)
{
	unsigned long *entries;
	int i;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !page)
		return;

	if (page_to_nid(page) != node) {
		pr_err("MITOSIS VERIFY cache_pop: pfn=0x%lx on node %d "
		       "but requested node %d\n",
		       page_to_pfn(page), page_to_nid(page), node);
		BUG();
	}

	if (page->pt_owner_mm) {
		pr_err("MITOSIS VERIFY cache_pop: pfn=0x%lx still has "
		       "pt_owner_mm=%px\n",
		       page_to_pfn(page), page->pt_owner_mm);
		BUG();
	}

	if (page->pt_replica) {
		pr_err("MITOSIS VERIFY cache_pop: pfn=0x%lx still has "
		       "pt_replica=%px\n",
		       page_to_pfn(page), page->pt_replica);
		BUG();
	}

	if (!PageMitosisFromCache(page)) {
		pr_err("MITOSIS VERIFY cache_pop: pfn=0x%lx missing "
		       "MitosisFromCache flag\n",
		       page_to_pfn(page));
		BUG();
	}

	entries = (unsigned long *)page_address(page);
	for (i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++) {
		if (entries[i] != 0) {
			pr_err("MITOSIS VERIFY cache_pop: pfn=0x%lx not "
			       "zeroed at entry[%d] val=0x%lx node=%d\n",
			       page_to_pfn(page), i, entries[i], node);
			BUG();
		}
	}
}

void mitosis_verify_mm_coherence(struct mm_struct *mm)
{
	int i;
	int primary_node;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm ||
	    !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	if (nodes_empty(mm->repl_pgd_nodes)) {
		pr_err("MITOSIS VERIFY coherence: enabled but "
		       "repl_pgd_nodes empty mm=%px\n", mm);
		BUG();
	}

	if (nodes_weight(mm->repl_pgd_nodes) < 2) {
		pr_err("MITOSIS VERIFY coherence: enabled but only "
		       "%d nodes mm=%px\n",
		       nodes_weight(mm->repl_pgd_nodes), mm);
		BUG();
	}

	if (!mm->original_pgd) {
		pr_err("MITOSIS VERIFY coherence: enabled but "
		       "original_pgd NULL mm=%px\n", mm);
		BUG();
	}

	if (mm->pgd != mm->original_pgd) {
		pr_err("MITOSIS VERIFY coherence: pgd=%px diverged "
		       "from original_pgd=%px mm=%px\n",
		       mm->pgd, mm->original_pgd, mm);
		BUG();
	}

	primary_node = page_to_nid(virt_to_page(mm->original_pgd));

	if (!node_isset(primary_node, mm->repl_pgd_nodes)) {
		pr_err("MITOSIS VERIFY coherence: primary node %d "
		       "not in repl_pgd_nodes mm=%px\n",
		       primary_node, mm);
		BUG();
	}

	if (mm->pgd_replicas[primary_node] != mm->original_pgd) {
		pr_err("MITOSIS VERIFY coherence: pgd_replicas[%d]=%px "
		       "!= original_pgd=%px mm=%px\n",
		       primary_node, mm->pgd_replicas[primary_node],
		       mm->original_pgd, mm);
		BUG();
	}

	for_each_node_mask(i, mm->repl_pgd_nodes) {
		if (!mm->pgd_replicas[i]) {
			pr_err("MITOSIS VERIFY coherence: pgd_replicas[%d] "
			       "NULL but node in mask mm=%px\n", i, mm);
			BUG();
		}

		if (page_to_nid(virt_to_page(mm->pgd_replicas[i])) != i) {
			pr_err("MITOSIS VERIFY coherence: pgd_replicas[%d] "
			       "on wrong node %d mm=%px\n",
			       i, page_to_nid(virt_to_page(mm->pgd_replicas[i])),
			       mm);
			BUG();
		}
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (!node_isset(i, mm->repl_pgd_nodes) &&
		    mm->pgd_replicas[i]) {
			pr_err("MITOSIS VERIFY coherence: pgd_replicas[%d] "
			       "non-NULL but node not in mask mm=%px\n",
			       i, mm);
			BUG();
		}
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		int target = READ_ONCE(mm->repl_steering[i]);

		if (target == -1)
			continue;

		if (target < 0 || target >= NUMA_NODE_COUNT) {
			pr_err("MITOSIS VERIFY coherence: steering[%d]=%d "
			       "out of range mm=%px\n", i, target, mm);
			BUG();
		}

		if (!node_isset(target, mm->repl_pgd_nodes)) {
			pr_err("MITOSIS VERIFY coherence: steering[%d]=%d "
			       "but node not in mask mm=%px\n",
			       i, target, mm);
			BUG();
		}

		if (!mm->pgd_replicas[target]) {
			pr_err("MITOSIS VERIFY coherence: steering[%d]=%d "
			       "but pgd_replicas[%d] NULL mm=%px\n",
			       i, target, target, mm);
			BUG();
		}
	}
}

void mitosis_verify_after_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page, *cur;
	unsigned long offset;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	cur = READ_ONCE(pte_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	pn = page_to_nid(pte_page);

	while (cur != pte_page) {
		pte_t *re = (pte_t *)(page_address(cur) + offset);
		pte_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pte_present(pteval)) {
			if (!pte_present(rv)) {
				pr_err("MITOSIS VERIFY set_pte: present on n%d "
				       "but not on n%d pfn=0x%lx off=%lu\n",
				       pn, rn, pte_pfn(pteval), offset);
				BUG();
			}
			if (pte_pfn(pteval) != pte_pfn(rv)) {
				pr_err("MITOSIS VERIFY set_pte: pfn mismatch "
				       "n%d=0x%lx n%d=0x%lx off=%lu\n",
				       pn, pte_pfn(pteval), rn, pte_pfn(rv), offset);
				BUG();
			}
			if (pte_write(pteval) != pte_write(rv)) {
				pr_err("MITOSIS VERIFY set_pte: write mismatch "
				       "n%d=%d n%d=%d pfn=0x%lx off=%lu\n",
				       pn, pte_write(pteval), rn, pte_write(rv),
				       pte_pfn(pteval), offset);
				BUG();
			}
		} else if (pte_present(rv)) {
			pr_err("MITOSIS VERIFY set_pte: cleared on n%d "
			       "but present on n%d replica_pfn=0x%lx off=%lu\n",
			       pn, rn, pte_pfn(rv), offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_set_pmd(pmd_t *pmdp, pmd_t pmdval)
{
	struct page *parent_page, *cur;
	unsigned long offset;
	unsigned long entry_val;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	parent_page = virt_to_page(pmdp);
	cur = READ_ONCE(parent_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	entry_val = pmd_val(pmdval);
	pn = page_to_nid(parent_page);

	while (cur != parent_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pmd_present(pmdval) && (pmd_trans_huge(pmdval) || pmd_leaf(pmdval))) {
			if (!pmd_present(rv) || (!pmd_trans_huge(rv) && !pmd_leaf(rv))) {
				pr_err("MITOSIS VERIFY set_pmd: THP on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			if (pmd_pfn(pmdval) != pmd_pfn(rv)) {
				pr_err("MITOSIS VERIFY set_pmd: THP pfn mismatch "
				       "n%d=0x%lx n%d=0x%lx off=%lu\n",
				       pn, pmd_pfn(pmdval), rn, pmd_pfn(rv), offset);
				BUG();
			}
			if (pmd_write(pmdval) != pmd_write(rv)) {
				pr_err("MITOSIS VERIFY set_pmd: THP write mismatch "
				       "n%d=%d n%d=%d off=%lu\n",
				       pn, pmd_write(pmdval), rn, pmd_write(rv), offset);
				BUG();
			}
		} else if (pmd_present(pmdval) && entry_val != 0) {
			unsigned long child_phys = pmd_val(rv) & PTE_PFN_MASK;
			struct page *child_page;

			if (!pmd_present(rv)) {
				pr_err("MITOSIS VERIFY set_pmd: present on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			if (child_phys) {
				child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
				if (page_to_nid(child_page) != rn) {
					pr_err("MITOSIS VERIFY set_pmd: PTE child on n%d "
					       "inside PMD replica on n%d off=%lu\n",
					       page_to_nid(child_page), rn, offset);
					BUG();
				}
			}
		} else if (pmd_present(rv)) {
			pr_err("MITOSIS VERIFY set_pmd: cleared on n%d "
			       "but present on n%d off=%lu\n", pn, rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_set_pud(pud_t *pudp, pud_t pudval)
{
	struct page *parent_page, *cur;
	unsigned long offset;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pudp || !virt_addr_valid(pudp))
		return;

	parent_page = virt_to_page(pudp);
	cur = READ_ONCE(parent_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pudp) & ~PAGE_MASK;
	pn = page_to_nid(parent_page);

	while (cur != parent_page) {
		pud_t *re = (pud_t *)(page_address(cur) + offset);
		pud_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pud_present(pudval) && !pud_trans_huge(pudval)) {
			unsigned long child_phys;

			if (!pud_present(rv)) {
				pr_err("MITOSIS VERIFY set_pud: present on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			child_phys = pud_val(rv) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (page_to_nid(cp) != rn) {
					pr_err("MITOSIS VERIFY set_pud: PMD child on n%d "
					       "inside PUD replica on n%d off=%lu\n",
					       page_to_nid(cp), rn, offset);
					BUG();
				}
			}
		} else if (!pud_present(pudval)) {
			if (pud_present(rv)) {
				pr_err("MITOSIS VERIFY set_pud: cleared on n%d "
				       "but present on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_set_p4d(p4d_t *p4dp, p4d_t p4dval)
{
	struct page *parent_page, *cur;
	unsigned long offset;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !p4dp || !virt_addr_valid(p4dp))
		return;

	parent_page = virt_to_page(p4dp);
	cur = READ_ONCE(parent_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)p4dp) & ~PAGE_MASK;
	pn = page_to_nid(parent_page);

	while (cur != parent_page) {
		p4d_t *re = (p4d_t *)(page_address(cur) + offset);
		p4d_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (p4d_present(p4dval)) {
			unsigned long child_phys;

			if (!p4d_present(rv)) {
				pr_err("MITOSIS VERIFY set_p4d: present on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			child_phys = p4d_val(rv) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (page_to_nid(cp) != rn) {
					pr_err("MITOSIS VERIFY set_p4d: PUD child on n%d "
					       "inside P4D replica on n%d off=%lu\n",
					       page_to_nid(cp), rn, offset);
					BUG();
				}
			}
		} else if (p4d_present(rv)) {
			pr_err("MITOSIS VERIFY set_p4d: cleared on n%d "
			       "but present on n%d off=%lu\n", pn, rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_set_pgd(pgd_t *pgdp, pgd_t pgdval)
{
	struct page *parent_page, *cur;
	unsigned long offset;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pgdp || !virt_addr_valid(pgdp))
		return;

	parent_page = virt_to_page(pgdp);
	cur = READ_ONCE(parent_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pgdp) & ~PAGE_MASK;
	pn = page_to_nid(parent_page);

	while (cur != parent_page) {
		pgd_t *re = (pgd_t *)(page_address(cur) + offset);
		pgd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pgd_present(pgdval)) {
			unsigned long child_phys;

			if (!pgd_present(rv)) {
				pr_err("MITOSIS VERIFY set_pgd: present on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			child_phys = pgd_val(rv) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (page_to_nid(cp) != rn) {
					pr_err("MITOSIS VERIFY set_pgd: child on n%d "
					       "inside PGD replica on n%d off=%lu\n",
					       page_to_nid(cp), rn, offset);
					BUG();
				}
			}
		} else if (pgd_present(rv)) {
			pr_err("MITOSIS VERIFY set_pgd: cleared on n%d "
			       "but present on n%d off=%lu\n", pn, rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_thp_split(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *pmd_page, *pmd_cur;
	struct page *primary_pte_page;
	pte_t *primary_ptes;
	unsigned long pmd_offset;
	unsigned long pte_phys;
	pmd_t pmval;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	if (!READ_ONCE(pmd_page->pt_replica))
		return;

	pmd_offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	pmval = READ_ONCE(*pmdp);

	if (pmd_trans_huge(pmval) || pmd_leaf(pmval)) {
		pr_err("MITOSIS VERIFY thp_split: primary PMD still huge "
		       "pmdval=0x%lx\n", (unsigned long)pmd_val(pmval));
		BUG();
	}

	if (!pmd_present(pmval) || pmd_val(pmval) == 0) {
		pr_err("MITOSIS VERIFY thp_split: primary PMD not present "
		       "after split\n");
		BUG();
	}

	pte_phys = pmd_val(pmval) & PTE_PFN_MASK;
	primary_pte_page = pfn_to_page(pte_phys >> PAGE_SHIFT);
	primary_ptes = (pte_t *)page_address(primary_pte_page);

	pmd_cur = READ_ONCE(pmd_page->pt_replica);
	while (pmd_cur && pmd_cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(pmd_cur) + pmd_offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(pmd_cur);
		unsigned long child_phys;
		struct page *child_pte_page;
		pte_t *replica_ptes;
		int i;

		if (pmd_trans_huge(rv) || pmd_leaf(rv)) {
			pr_err("MITOSIS VERIFY thp_split: replica PMD on n%d "
			       "still huge after split\n", rn);
			BUG();
		}

		if (!pmd_present(rv)) {
			pr_err("MITOSIS VERIFY thp_split: replica PMD on n%d "
			       "not present after split\n", rn);
			BUG();
		}

		child_phys = pmd_val(rv) & PTE_PFN_MASK;
		child_pte_page = pfn_to_page(child_phys >> PAGE_SHIFT);

		if (page_to_nid(child_pte_page) != rn) {
			pr_err("MITOSIS VERIFY thp_split: PTE child on n%d "
			       "inside PMD replica on n%d\n",
			       page_to_nid(child_pte_page), rn);
			BUG();
		}

		replica_ptes = (pte_t *)page_address(child_pte_page);

		for (i = 0; i < PTRS_PER_PTE; i++) {
			pte_t p = READ_ONCE(primary_ptes[i]);
			pte_t r = READ_ONCE(replica_ptes[i]);

			if (pte_present(p) != pte_present(r)) {
				pr_err("MITOSIS VERIFY thp_split: PTE[%d] present "
				       "mismatch primary=%d replica(n%d)=%d\n",
				       i, pte_present(p), rn, pte_present(r));
				BUG();
			}

			if (pte_present(p) && pte_pfn(p) != pte_pfn(r)) {
				pr_err("MITOSIS VERIFY thp_split: PTE[%d] pfn "
				       "mismatch primary=0x%lx replica(n%d)=0x%lx\n",
				       i, pte_pfn(p), rn, pte_pfn(r));
				BUG();
			}
		}

		pmd_cur = READ_ONCE(pmd_cur->pt_replica);
	}
	mitosis_verify_chain_integrity(pmd_page, mm, MITOSIS_CACHE_PMD);
	mitosis_verify_chain_integrity(primary_pte_page, mm, MITOSIS_CACHE_PTE);
}

void mitosis_verify_after_ptep_get_and_clear(pte_t *ptep)
{
	struct page *pte_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	cur = READ_ONCE(pte_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	if (pte_val(READ_ONCE(*ptep)) != 0) {
		pr_err("MITOSIS VERIFY ptep_get_and_clear: primary not zero "
		       "val=0x%lx off=%lu\n",
		       (unsigned long)pte_val(READ_ONCE(*ptep)), offset);
		BUG();
	}

	while (cur != pte_page) {
		pte_t *re = (pte_t *)(page_address(cur) + offset);
		pte_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pte_val(rv) != 0) {
			pr_err("MITOSIS VERIFY ptep_get_and_clear: replica on n%d "
			       "not zero val=0x%lx off=%lu\n",
			       rn, (unsigned long)pte_val(rv), offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_ptep_set_wrprotect(pte_t *ptep)
{
	struct page *pte_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	cur = READ_ONCE(pte_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	if (pte_present(READ_ONCE(*ptep)) && pte_write(READ_ONCE(*ptep))) {
		pr_err("MITOSIS VERIFY ptep_set_wrprotect: primary still writable "
		       "off=%lu\n", offset);
		BUG();
	}

	while (cur != pte_page) {
		pte_t *re = (pte_t *)(page_address(cur) + offset);
		pte_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pte_present(rv) && pte_write(rv)) {
			pr_err("MITOSIS VERIFY ptep_set_wrprotect: replica on n%d "
			       "still writable pfn=0x%lx off=%lu\n",
			       rn, pte_pfn(rv), offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_pmdp_set_wrprotect(pmd_t *pmdp)
{
	struct page *pmd_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	cur = READ_ONCE(pmd_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (pmd_present(READ_ONCE(*pmdp)) && pmd_write(READ_ONCE(*pmdp))) {
		pr_err("MITOSIS VERIFY pmdp_set_wrprotect: primary still writable "
		       "off=%lu\n", offset);
		BUG();
	}

	while (cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pmd_present(rv) && pmd_write(rv)) {
			pr_err("MITOSIS VERIFY pmdp_set_wrprotect: replica on n%d "
			       "still writable off=%lu\n", rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_pmdp_huge_get_and_clear(pmd_t *pmdp)
{
	struct page *pmd_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	cur = READ_ONCE(pmd_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (pmd_val(READ_ONCE(*pmdp)) != 0) {
		pr_err("MITOSIS VERIFY pmdp_huge_get_and_clear: primary not zero "
		       "val=0x%lx off=%lu\n",
		       (unsigned long)pmd_val(READ_ONCE(*pmdp)), offset);
		BUG();
	}

	while (cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pmd_val(rv) != 0) {
			pr_err("MITOSIS VERIFY pmdp_huge_get_and_clear: replica on n%d "
			       "not zero val=0x%lx off=%lu\n",
			       rn, (unsigned long)pmd_val(rv), offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_pmdp_establish(pmd_t *pmdp, pmd_t newpmd)
{
	struct page *pmd_page, *cur;
	unsigned long offset;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	cur = READ_ONCE(pmd_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	pn = page_to_nid(pmd_page);

	while (cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pmd_present(newpmd) && (pmd_trans_huge(newpmd) || pmd_leaf(newpmd))) {
			if (!pmd_present(rv) || (!pmd_trans_huge(rv) && !pmd_leaf(rv))) {
				pr_err("MITOSIS VERIFY pmdp_establish: THP on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			if (pmd_pfn(newpmd) != pmd_pfn(rv)) {
				pr_err("MITOSIS VERIFY pmdp_establish: THP pfn mismatch "
				       "n%d=0x%lx n%d=0x%lx off=%lu\n",
				       pn, pmd_pfn(newpmd), rn, pmd_pfn(rv), offset);
				BUG();
			}
		} else if (pmd_present(newpmd)) {
			unsigned long child_phys;

			if (!pmd_present(rv)) {
				pr_err("MITOSIS VERIFY pmdp_establish: present on n%d "
				       "but not on n%d off=%lu\n", pn, rn, offset);
				BUG();
			}
			child_phys = pmd_val(rv) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *cp = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (page_to_nid(cp) != rn) {
					pr_err("MITOSIS VERIFY pmdp_establish: PTE child "
					       "on n%d inside PMD replica on n%d off=%lu\n",
					       page_to_nid(cp), rn, offset);
					BUG();
				}
			}
		} else if (pmd_present(rv)) {
			pr_err("MITOSIS VERIFY pmdp_establish: cleared on n%d "
			       "but present on n%d off=%lu\n", pn, rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_deposit(struct mm_struct *mm, pmd_t *pmdp,
				  pgtable_t pgtable)
{
	struct page *pmd_page, *pte_page;
	int pmd_node, pte_node;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) || !pgtable ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pmd_page = virt_to_page(pmdp);
	pte_page = pgtable;
	pmd_node = page_to_nid(pmd_page);
	pte_node = page_to_nid(pte_page);

	if (pmd_node != pte_node) {
		pr_err("MITOSIS VERIFY deposit: PTE page on n%d deposited "
		       "under PMD on n%d\n", pte_node, pmd_node);
		BUG();
	}
}

void mitosis_verify_after_withdraw(struct mm_struct *mm, pmd_t *pmdp,
				   pgtable_t pgtable)
{
	struct page *pmd_page, *pte_page;
	int pmd_node, pte_node;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	if (!pgtable) {
		pr_err("MITOSIS VERIFY withdraw: NULL page withdrawn from PMD\n");
		BUG();
	}

	pmd_page = virt_to_page(pmdp);
	pte_page = pgtable;
	pmd_node = page_to_nid(pmd_page);
	pte_node = page_to_nid(pte_page);

	if (pmd_node != pte_node) {
		pr_err("MITOSIS VERIFY withdraw: PTE page on n%d withdrawn "
		       "from PMD on n%d\n", pte_node, pmd_node);
		BUG();
	}
}

void mitosis_verify_get_pte(pte_t *ptep, pte_t result)
{
}

void mitosis_verify_get_pmd(pmd_t *pmdp, pmd_t result)
{
}

void mitosis_verify_after_ptep_clear_young(pte_t *ptep)
{
	struct page *pte_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !ptep || !virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);
	cur = READ_ONCE(pte_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;

	if (pte_young(READ_ONCE(*ptep))) {
		pr_err("MITOSIS VERIFY ptep_clear_young: primary still young "
		       "off=%lu\n", offset);
		BUG();
	}

	while (cur != pte_page) {
		pte_t *re = (pte_t *)(page_address(cur) + offset);
		int rn = page_to_nid(cur);

		if (pte_young(READ_ONCE(*re))) {
			pr_err("MITOSIS VERIFY ptep_clear_young: replica on n%d "
			       "still young off=%lu\n", rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_pmdp_clear_young(pmd_t *pmdp)
{
	struct page *pmd_page, *cur;
	unsigned long offset;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp))
		return;

	pmd_page = virt_to_page(pmdp);
	cur = READ_ONCE(pmd_page->pt_replica);
	if (!cur)
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (pmd_present(READ_ONCE(*pmdp)) && pmd_young(READ_ONCE(*pmdp))) {
		pr_err("MITOSIS VERIFY pmdp_clear_young: primary still young "
		       "off=%lu\n", offset);
		BUG();
	}

	while (cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (pmd_present(rv) && pmd_trans_huge(rv) && pmd_young(rv)) {
			pr_err("MITOSIS VERIFY pmdp_clear_young: replica on n%d "
			       "still young off=%lu\n", rn, offset);
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
}

void mitosis_verify_after_repl_alloc(struct mm_struct *mm, unsigned long pfn,
				     int level)
{
	struct page *primary, *cur;
	int pn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !pfn_valid(pfn) ||
	    !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	primary = pfn_to_page(pfn);
	cur = READ_ONCE(primary->pt_replica);
	if (!cur)
		return;

	mitosis_verify_chain_integrity(primary, mm, level);

	pn = page_to_nid(primary);
	cur = READ_ONCE(primary->pt_replica);

	while (cur != primary) {
		unsigned long *src = (unsigned long *)page_address(primary);
		unsigned long *dst = (unsigned long *)page_address(cur);
		int rn = page_to_nid(cur);
		int i;

		for (i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++) {
			unsigned long s = READ_ONCE(src[i]);
			unsigned long d = READ_ONCE(dst[i]);

			if (s & _PAGE_PRESENT) {
				if ((d & ~_PAGE_ACCESSED) !=
				    (s & ~_PAGE_ACCESSED)) {
					pr_err("MITOSIS VERIFY repl_alloc: content "
					       "mismatch n%d vs n%d entry[%d] "
					       "src=0x%lx dst=0x%lx level=%d\n",
					       pn, rn, i, s, d, level);
					BUG();
				}
			}
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur) {
			pr_err("MITOSIS VERIFY repl_alloc: chain broken "
			       "level=%d pfn=0x%lx\n", level, pfn);
			BUG();
		}
	}
}

void mitosis_verify_after_thp_collapse(struct mm_struct *mm, pmd_t *pmdp)
{
	struct page *pmd_page, *cur, *head;
	unsigned long offset;
	pmd_t pmval;
	unsigned long primary_pfn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !pmdp || !virt_addr_valid(pmdp) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pmd_page = virt_to_page(pmdp);
	if (!READ_ONCE(pmd_page->pt_replica))
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;
	pmval = READ_ONCE(*pmdp);

	if (!pmd_present(pmval) || !pmd_trans_huge(pmval)) {
		pr_err("MITOSIS VERIFY thp_collapse: primary PMD not THP "
		       "val=0x%lx\n", (unsigned long)pmd_val(pmval));
		BUG();
	}

	primary_pfn = pmd_pfn(pmval);
	head = pfn_to_page(primary_pfn);

	if (!PageCompound(head)) {
		pr_err("MITOSIS VERIFY thp_collapse: page pfn=0x%lx "
		       "not compound\n", primary_pfn);
		BUG();
	}

	if (compound_order(compound_head(head)) != HPAGE_PMD_ORDER) {
		pr_err("MITOSIS VERIFY thp_collapse: page pfn=0x%lx "
		       "order=%u expected=%d\n",
		       primary_pfn, compound_order(compound_head(head)),
		       HPAGE_PMD_ORDER);
		BUG();
	}

	cur = READ_ONCE(pmd_page->pt_replica);
	while (cur != pmd_page) {
		pmd_t *re = (pmd_t *)(page_address(cur) + offset);
		pmd_t rv = READ_ONCE(*re);
		int rn = page_to_nid(cur);

		if (!pmd_present(rv) || !pmd_trans_huge(rv)) {
			pr_err("MITOSIS VERIFY thp_collapse: replica on n%d "
			       "not THP val=0x%lx\n",
			       rn, (unsigned long)pmd_val(rv));
			BUG();
		}

		if (pmd_pfn(rv) != primary_pfn) {
			pr_err("MITOSIS VERIFY thp_collapse: pfn mismatch "
			       "primary=0x%lx replica(n%d)=0x%lx\n",
			       primary_pfn, rn, pmd_pfn(rv));
			BUG();
		}

		cur = READ_ONCE(cur->pt_replica);
		if (!cur)
			break;
	}
	mitosis_verify_chain_integrity(pmd_page, mm, MITOSIS_CACHE_PMD);
}

static void verify_enable_walk_tree(struct mm_struct *mm, const char *caller)
{
	pgd_t *pgd = mm->pgd;
	int gi, p4i, pui, pmi;

	for (gi = 0; gi < KERNEL_PGD_BOUNDARY; gi++) {
		pgd_t gval = READ_ONCE(pgd[gi]);
		p4d_t *p4d_base;

		if (pgd_none(gval) || !pgd_present(gval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long cphys = pgd_val(gval) & PTE_PFN_MASK;

			if (cphys) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (!READ_ONCE(p->pt_replica)) {
					pr_err("MITOSIS VERIFY enable: P4D at PGD[%d] "
					       "has no replicas\n", gi);
					BUG();
				}
				mitosis_verify_chain_integrity(p, mm,
							       MITOSIS_CACHE_P4D);
			}
		}

		p4d_base = p4d_offset(&pgd[gi], 0);

		for (p4i = 0; p4i < PTRS_PER_P4D; p4i++) {
			p4d_t p4val = READ_ONCE(p4d_base[p4i]);
			pud_t *pud_base;
			unsigned long cphys;

			if (p4d_none(p4val) || !p4d_present(p4val))
				continue;

			cphys = p4d_val(p4val) & PTE_PFN_MASK;
			if (cphys) {
				struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

				if (!READ_ONCE(p->pt_replica)) {
					pr_err("MITOSIS VERIFY enable: PUD at [%d][%d] "
					       "has no replicas\n", gi, p4i);
					BUG();
				}
				mitosis_verify_chain_integrity(p, mm,
							       MITOSIS_CACHE_PUD);
			}

			pud_base = pud_offset(&p4d_base[p4i], 0);

			for (pui = 0; pui < PTRS_PER_PUD; pui++) {
				pud_t puval = READ_ONCE(pud_base[pui]);
				pmd_t *pmd_base;

				if (pud_none(puval) || !pud_present(puval) ||
				    pud_trans_huge(puval))
					continue;

				cphys = pud_val(puval) & PTE_PFN_MASK;
				if (cphys) {
					struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

					if (!READ_ONCE(p->pt_replica)) {
						pr_err("MITOSIS VERIFY enable: PMD at [%d][%d][%d] "
						       "has no replicas\n", gi, p4i, pui);
						BUG();
					}
					mitosis_verify_chain_integrity(p, mm,
								       MITOSIS_CACHE_PMD);
				}

				pmd_base = pmd_offset(&pud_base[pui], 0);

				for (pmi = 0; pmi < PTRS_PER_PMD; pmi++) {
					pmd_t pmval = READ_ONCE(pmd_base[pmi]);

					if (pmd_none(pmval) || !pmd_present(pmval) ||
					    pmd_trans_huge(pmval) || pmd_leaf(pmval))
						continue;

					cphys = pmd_val(pmval) & PTE_PFN_MASK;
					if (cphys) {
						struct page *p = pfn_to_page(cphys >> PAGE_SHIFT);

						if (!READ_ONCE(p->pt_replica)) {
							pr_err("MITOSIS VERIFY enable: PTE at "
							       "[%d][%d][%d][%d] has no replicas\n",
							       gi, p4i, pui, pmi);
							BUG();
						}
						mitosis_verify_chain_integrity(p, mm,
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
	if (!READ_ONCE(pgd_page->pt_replica)) {
		pr_err("MITOSIS VERIFY enable: PGD has no replicas\n");
		BUG();
	}

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		if (!mm->pgd_replicas[node]) {
			pr_err("MITOSIS VERIFY enable: pgd_replicas[%d] is NULL\n",
			       node);
			BUG();
		}
		if (page_to_nid(virt_to_page(mm->pgd_replicas[node])) != node) {
			pr_err("MITOSIS VERIFY enable: pgd_replicas[%d] on wrong "
			       "node %d\n", node,
			       page_to_nid(virt_to_page(mm->pgd_replicas[node])));
			BUG();
		}
	}

	verify_enable_walk_tree(mm, "enable");

	mitosis_verify_tree_consistency(mm);

	mitosis_verify_pti_consistency(mm);

	mitosis_verify_mm_coherence(mm);
}

static void verify_disable_walk_tree(struct mm_struct *mm)
{
	pgd_t *pgd = mm->pgd;
	int gi, p4i, pui, pmi;

	for (gi = 0; gi < KERNEL_PGD_BOUNDARY; gi++) {
		pgd_t gval = READ_ONCE(pgd[gi]);
		p4d_t *p4d_base;

		if (pgd_none(gval) || !pgd_present(gval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long cphys = pgd_val(gval) & PTE_PFN_MASK;

			if (cphys && READ_ONCE(pfn_to_page(cphys >> PAGE_SHIFT)->pt_replica)) {
				pr_err("MITOSIS VERIFY disable: P4D at PGD[%d] "
				       "still has replicas\n", gi);
				BUG();
			}
		}

		p4d_base = p4d_offset(&pgd[gi], 0);

		for (p4i = 0; p4i < PTRS_PER_P4D; p4i++) {
			p4d_t p4val = READ_ONCE(p4d_base[p4i]);
			pud_t *pud_base;
			unsigned long cphys;

			if (p4d_none(p4val) || !p4d_present(p4val))
				continue;

			cphys = p4d_val(p4val) & PTE_PFN_MASK;
			if (cphys && READ_ONCE(pfn_to_page(cphys >> PAGE_SHIFT)->pt_replica)) {
				pr_err("MITOSIS VERIFY disable: PUD at [%d][%d] "
				       "still has replicas\n", gi, p4i);
				BUG();
			}

			pud_base = pud_offset(&p4d_base[p4i], 0);

			for (pui = 0; pui < PTRS_PER_PUD; pui++) {
				pud_t puval = READ_ONCE(pud_base[pui]);
				pmd_t *pmd_base;

				if (pud_none(puval) || !pud_present(puval) ||
				    pud_trans_huge(puval))
					continue;

				cphys = pud_val(puval) & PTE_PFN_MASK;
				if (cphys && READ_ONCE(pfn_to_page(cphys >> PAGE_SHIFT)->pt_replica)) {
					pr_err("MITOSIS VERIFY disable: PMD at [%d][%d][%d] "
					       "still has replicas\n", gi, p4i, pui);
					BUG();
				}

				pmd_base = pmd_offset(&pud_base[pui], 0);

				for (pmi = 0; pmi < PTRS_PER_PMD; pmi++) {
					pmd_t pmval = READ_ONCE(pmd_base[pmi]);

					if (pmd_none(pmval) || !pmd_present(pmval) ||
					    pmd_trans_huge(pmval) || pmd_leaf(pmval))
						continue;

					cphys = pmd_val(pmval) & PTE_PFN_MASK;
					if (cphys && READ_ONCE(pfn_to_page(cphys >> PAGE_SHIFT)->pt_replica)) {
						pr_err("MITOSIS VERIFY disable: PTE at "
						       "[%d][%d][%d][%d] still has replicas\n",
						       gi, p4i, pui, pmi);
						BUG();
					}
				}
			}
		}
	}
}

void mitosis_verify_after_disable(struct mm_struct *mm)
{
	struct page *pgd_page;
	int i;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !mm)
		return;

	if (mm->repl_pgd_enabled) {
		pr_err("MITOSIS VERIFY disable: repl_pgd_enabled still true\n");
		BUG();
	}

	if (!nodes_empty(mm->repl_pgd_nodes)) {
		pr_err("MITOSIS VERIFY disable: repl_pgd_nodes not empty\n");
		BUG();
	}

	if (mm->original_pgd) {
		pr_err("MITOSIS VERIFY disable: original_pgd not NULL\n");
		BUG();
	}

	pgd_page = virt_to_page(mm->pgd);
	if (READ_ONCE(pgd_page->pt_replica)) {
		pr_err("MITOSIS VERIFY disable: PGD still has replicas\n");
		BUG();
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (mm->pgd_replicas[i]) {
			pr_err("MITOSIS VERIFY disable: pgd_replicas[%d] not NULL\n", i);
			BUG();
		}
	}

	verify_disable_walk_tree(mm);
}

void mitosis_verify_after_cr3_switch(struct mm_struct *mm)
{
	unsigned long cr3_pa;
	struct page *cr3_page;
	int cr3_node, local_node, target_node;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress))
		return;

	cr3_pa = __read_cr3() & PAGE_MASK;
	if (!cr3_pa || !pfn_valid(cr3_pa >> PAGE_SHIFT))
		return;

	cr3_page = pfn_to_page(cr3_pa >> PAGE_SHIFT);
	cr3_node = page_to_nid(cr3_page);
	local_node = numa_node_id();

	target_node = READ_ONCE(mm->repl_steering[local_node]);
	if (target_node < 0 || target_node >= NUMA_NODE_COUNT)
		target_node = local_node;

	if (!mm->pgd_replicas[target_node]) {
		if (cr3_pa != __pa(mm->pgd))
			return;
	} else if (cr3_pa != __pa(mm->pgd_replicas[target_node])) {
		pr_err("MITOSIS VERIFY cr3_switch: CPU on n%d target n%d "
		       "cr3=0x%lx expected=0x%lx (cr3 on n%d)\n",
		       local_node, target_node, cr3_pa,
		       (unsigned long)__pa(mm->pgd_replicas[target_node]),
		       cr3_node);
		BUG();
	}

	if (cr3_node != target_node) {
		pr_err("MITOSIS VERIFY cr3_switch: CR3 page on n%d "
		       "but CPU on n%d target n%d\n",
		       cr3_node, local_node, target_node);
		BUG();
	}
}

void mitosis_verify_after_free_replicas(struct page *primary, int level)
{
	if (!READ_ONCE(sysctl_mitosis_verify_enabled) || !primary)
		return;

	if (READ_ONCE(primary->pt_replica)) {
		pr_err("MITOSIS VERIFY free_replicas: pt_replica not NULL "
		       "after free level=%d pfn=0x%lx\n",
		       level, page_to_pfn(primary));
		BUG();
	}
}

void mitosis_verify_fault_locality(struct mm_struct *mm, unsigned long address)
{
	int node;
	unsigned long cr3_pa;
	pgd_t *pgd, gval;
	p4d_t *p4dp, p4val;
	pud_t *pudp, puval;
	pmd_t *pmdp, pmval;
	unsigned long cphys;
	int cn;

	if (!READ_ONCE(sysctl_mitosis_verify_enabled) ||
	    !mm || !smp_load_acquire(&mm->repl_pgd_enabled) ||
	    smp_load_acquire(&mm->repl_in_progress) ||
	    address >= TASK_SIZE)
		return;

	node = numa_node_id();
	cr3_pa = __read_cr3() & PAGE_MASK;
	if (!cr3_pa || !pfn_valid(cr3_pa >> PAGE_SHIFT))
		return;

	cn = page_to_nid(pfn_to_page(cr3_pa >> PAGE_SHIFT));
	if (cn != node) {
		pr_err("MITOSIS VERIFY fault: CR3 PGD on n%d "
		       "but CPU on n%d addr=0x%lx\n",
		       cn, node, address);
		BUG();
	}

	pgd = __va(cr3_pa);
	gval = READ_ONCE(pgd[pgd_index(address)]);
	if (pgd_none(gval) || !pgd_present(gval))
		return;

	if (pgtable_l5_enabled()) {
		cphys = pgd_val(gval) & PTE_PFN_MASK;
		if (cphys) {
			cn = page_to_nid(pfn_to_page(cphys >> PAGE_SHIFT));
			if (cn != node) {
				pr_err("MITOSIS VERIFY fault: P4D on n%d "
				       "expected n%d addr=0x%lx\n",
				       cn, node, address);
				BUG();
			}
		}
	}

	p4dp = p4d_offset(&pgd[pgd_index(address)], address);
	p4val = READ_ONCE(*p4dp);
	if (p4d_none(p4val) || !p4d_present(p4val))
		return;

	cphys = p4d_val(p4val) & PTE_PFN_MASK;
	if (cphys) {
		cn = page_to_nid(pfn_to_page(cphys >> PAGE_SHIFT));
		if (cn != node) {
			pr_err("MITOSIS VERIFY fault: PUD on n%d "
			       "expected n%d addr=0x%lx\n",
			       cn, node, address);
			BUG();
		}
	}

	pudp = pud_offset(p4dp, address);
	puval = READ_ONCE(*pudp);
	if (pud_none(puval) || !pud_present(puval) || pud_trans_huge(puval))
		return;

	cphys = pud_val(puval) & PTE_PFN_MASK;
	if (cphys) {
		cn = page_to_nid(pfn_to_page(cphys >> PAGE_SHIFT));
		if (cn != node) {
			pr_err("MITOSIS VERIFY fault: PMD on n%d "
			       "expected n%d addr=0x%lx\n",
			       cn, node, address);
			BUG();
		}
	}

	pmdp = pmd_offset(pudp, address);
	pmval = READ_ONCE(*pmdp);
	if (pmd_none(pmval) || !pmd_present(pmval) ||
	    pmd_trans_huge(pmval) || pmd_leaf(pmval))
		return;

	cphys = pmd_val(pmval) & PTE_PFN_MASK;
	if (cphys) {
		cn = page_to_nid(pfn_to_page(cphys >> PAGE_SHIFT));
		if (cn != node) {
			pr_err("MITOSIS VERIFY fault: PTE on n%d "
			       "expected n%d addr=0x%lx\n",
			       cn, node, address);
			BUG();
		}
	}
}
