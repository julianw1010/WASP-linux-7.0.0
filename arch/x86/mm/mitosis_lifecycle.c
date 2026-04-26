// SPDX-License-Identifier: GPL-2.0
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/pgtable_repl.h>
#include <asm/tlbflush.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/numa.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <asm/io.h>
#include <asm/pti.h>
#include <linux/task_work.h>
#include <linux/mempolicy.h>

int sysctl_mitosis_mode = -1;
int sysctl_mitosis_inherit = 1;

static DEFINE_MUTEX(global_repl_mutex);

struct cr3_switch_info {
	struct mm_struct *mm;
	pgd_t *original_pgd;
	int initiating_cpu;
};

bool link_page_replicas(struct page **pages, int count)
{
	int i;

	if (!pages || count < 2)
		return count < 2;

	for (i = 0; i < count; i++)
		WRITE_ONCE(pages[i]->pt_replica, NULL);

	/* Ensure NULLs are visible before building the ring */
	smp_wmb();

	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

	/* Ensure the full ring is visible to all CPUs */
	smp_mb();
	return true;
}

struct page *get_replica_for_node(struct page *base, int target_node)
{
	struct page *page;
	struct page *start_page;

	if (!base)
		return NULL;

	if (page_to_nid(base) == target_node)
		return base;

	page = READ_ONCE(base->pt_replica);
	if (!page)
		return NULL;

	start_page = base;

	while (page != start_page) {
		if (page_to_nid(page) == target_node)
			return page;
		page = READ_ONCE(page->pt_replica);
		if (!page)
			return NULL;
	}

	return NULL;
}

static bool replicate_and_link_page(struct page *page, struct mm_struct *mm,
				    int (*alloc_fn)(struct page *,
						   struct mm_struct *,
						   struct page **, int *),
				    const char *level_name)
{
	struct page *pages[NUMA_NODE_COUNT];
	int count = 0;
	void *src;
	int i, ret;

	if (!page || !mm || !alloc_fn || !mm->repl_in_progress)
		return false;

	if (READ_ONCE(page->pt_replica))
		return true;

	ret = alloc_fn(page, mm, pages, &count);
	if (ret != 0 || count < 2)
		return false;

	src = page_address(page);

	for (i = 1; i < count; i++) {
		unsigned long *entry;
		int j;

		memcpy(page_address(pages[i]), src, PAGE_SIZE);

		entry = (unsigned long *)page_address(pages[i]);
		for (j = 0; j < PAGE_SIZE / sizeof(unsigned long); j++) {
			if (entry[j] & _PAGE_PRESENT)
				entry[j] &= ~_PAGE_ACCESSED;
		}
	}

	/* Ensure replica contents are visible before linking */
	smp_wmb();

	if (WARN_ON_ONCE(!link_page_replicas(pages, count)))
		return false;

	/* Ensure ring is fully visible before proceeding */
	smp_mb();
	return true;
}

static void phase1_replicate_pmd_entries(pmd_t *pmd_base,
					 struct mm_struct *mm)
{
	int pmd_idx;

	for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
		pmd_t pmdval;
		unsigned long pte_phys;
		struct page *pte_page;

		if (!mm->repl_in_progress)
			return;

		pmdval = READ_ONCE(pmd_base[pmd_idx]);
		if (pmd_none(pmdval) || !pmd_present(pmdval) ||
		    pmd_trans_huge(pmdval))
			continue;

		pte_phys = pmd_val(pmdval) & PTE_PFN_MASK;
		if (!pte_phys)
			continue;

		pte_page = pfn_to_page(pte_phys >> PAGE_SHIFT);
		replicate_and_link_page(pte_page, mm,
					alloc_pte_replicas, "pte");
	}
}

static void phase1_replicate_pud_entries(pud_t *pud_base,
					 struct mm_struct *mm)
{
	int pud_idx;

	for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
		pud_t pudval;
		unsigned long pmd_phys;
		pmd_t *pmd_base;

		if (!mm->repl_in_progress)
			return;

		pudval = READ_ONCE(pud_base[pud_idx]);
		if (pud_none(pudval) || !pud_present(pudval) ||
		    pud_trans_huge(pudval))
			continue;

		pmd_phys = pud_val(pudval) & PTE_PFN_MASK;
		if (pmd_phys) {
			struct page *pmd_page;

			pmd_page = pfn_to_page(pmd_phys >> PAGE_SHIFT);
			replicate_and_link_page(pmd_page, mm,
						alloc_pmd_replicas, "pmd");
		}

		pmd_base = pmd_offset(&pud_base[pud_idx], 0);
		phase1_replicate_pmd_entries(pmd_base, mm);
	}
}

static void replicate_existing_pagetables_phase1(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx;

	if (!mm || !mm->repl_in_progress)
		return;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;
		int p4d_idx;

		if (!mm->repl_in_progress)
			return;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long child_phys;

			child_phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (child_phys) {
				struct page *child_page;

				child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
				replicate_and_link_page(child_page, mm,
							alloc_p4d_replicas,
							"p4d");
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			unsigned long pud_phys;
			pud_t *pud_base;

			if (!mm->repl_in_progress)
				return;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			pud_phys = p4d_val(p4dval) & PTE_PFN_MASK;
			if (pud_phys) {
				struct page *pud_page;

				pud_page = pfn_to_page(pud_phys >> PAGE_SHIFT);
				replicate_and_link_page(pud_page, mm,
							alloc_pud_replicas,
							"pud");
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);
			phase1_replicate_pud_entries(pud_base, mm);
		}
	}

	/* Ensure all phase1 replicas are visible before phase2 rewiring */
	smp_mb();
}

static void phase2_rewire_pmd_entries(pmd_t *pmd_base, struct mm_struct *mm,
				      int node)
{
	int pmd_idx;

	for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
		pmd_t pmdval;
		unsigned long child_phys;
		struct page *child_page, *local_child;

		if (!mm->repl_in_progress)
			return;

		pmdval = READ_ONCE(pmd_base[pmd_idx]);
		if (pmd_none(pmdval) || !pmd_present(pmdval) ||
		    pmd_trans_huge(pmdval))
			continue;

		child_phys = pmd_val(pmdval) & PTE_PFN_MASK;
		if (!child_phys)
			continue;

		child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		if (!READ_ONCE(child_page->pt_replica))
			continue;

		local_child = get_replica_for_node(child_page, node);
		if (local_child && page_to_nid(local_child) == node) {
			unsigned long new_phys;
			pmdval_t new_val;

			new_phys = __pa(page_address(local_child));
			new_val = new_phys |
				  (pmd_val(pmdval) & ~PTE_PFN_MASK);
			WRITE_ONCE(pmd_base[pmd_idx], __pmd(new_val));
		}
	}
}

static void phase2_rewire_pud_entries(pud_t *pud_base, struct mm_struct *mm,
				      int node, int primary_node)
{
	int pud_idx;

	for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
		pud_t pudval;
		unsigned long child_phys;
		struct page *child_page, *local_child;
		pmd_t *node_pmd_base;

		if (!mm->repl_in_progress)
			return;

		pudval = READ_ONCE(pud_base[pud_idx]);
		if (pud_none(pudval) || !pud_present(pudval) ||
		    pud_trans_huge(pudval))
			continue;

		child_phys = pud_val(pudval) & PTE_PFN_MASK;
		if (!child_phys)
			goto walk_pmds;

		child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		if (!READ_ONCE(child_page->pt_replica))
			goto walk_pmds;

		local_child = get_replica_for_node(child_page, node);
		if (local_child && page_to_nid(local_child) == node) {
			unsigned long new_phys;
			pudval_t new_val;

			new_phys = __pa(page_address(local_child));
			new_val = new_phys |
				  (pud_val(pudval) & ~PTE_PFN_MASK);

			if (node == primary_node &&
			    local_child != child_page) {
				struct ptdesc *orig;
				struct ptdesc *local;

				orig = page_ptdesc(child_page);
				local = page_ptdesc(local_child);
				if (orig->pmd_huge_pte &&
				    !local->pmd_huge_pte) {
					local->pmd_huge_pte =
						orig->pmd_huge_pte;
					orig->pmd_huge_pte = NULL;
				}
			}

			WRITE_ONCE(pud_base[pud_idx], __pud(new_val));
		}

walk_pmds:
		node_pmd_base = pmd_offset(&pud_base[pud_idx], 0);
		phase2_rewire_pmd_entries(node_pmd_base, mm, node);
	}
}

static void phase2_rewire_p4d_entries(p4d_t *p4d_base, struct mm_struct *mm,
				      int node, int primary_node)
{
	int p4d_idx;

	for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
		p4d_t p4dval;
		unsigned long child_phys;
		struct page *child_page, *local_child;
		pud_t *node_pud_base;

		if (!mm->repl_in_progress)
			return;

		p4dval = READ_ONCE(p4d_base[p4d_idx]);
		if (p4d_none(p4dval) || !p4d_present(p4dval))
			continue;

		child_phys = p4d_val(p4dval) & PTE_PFN_MASK;
		if (!child_phys)
			goto walk_puds;

		child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
		if (!READ_ONCE(child_page->pt_replica))
			goto walk_puds;

		local_child = get_replica_for_node(child_page, node);
		if (local_child && page_to_nid(local_child) == node) {
			unsigned long new_phys;
			p4dval_t new_val;

			new_phys = __pa(page_address(local_child));
			new_val = new_phys |
				  (p4d_val(p4dval) & ~PTE_PFN_MASK);
			WRITE_ONCE(p4d_base[p4d_idx], __p4d(new_val));
		}

walk_puds:
		node_pud_base = pud_offset(&p4d_base[p4d_idx], 0);
		phase2_rewire_pud_entries(node_pud_base, mm,
					 node, primary_node);
	}
}

static void phase2_rewire_pgd_pti(pgd_t *node_pgd, int pgd_idx,
				  unsigned long new_phys)
{
	pgd_t *user_entry;

	if (!mitosis_pti_active())
		return;

	user_entry = mitosis_get_user_pgd_entry(&node_pgd[pgd_idx]);
	if (!user_entry)
		return;

	WRITE_ONCE(*user_entry,
		   __pgd(new_phys |
			 (pgd_val(*user_entry) & ~PTE_PFN_MASK)));
}

static void replicate_existing_pagetables_phase2(struct mm_struct *mm)
{
	pgd_t *pgd;
	struct page *pgd_page;
	int node;
	int primary_node;

	if (!mm || !mm->repl_in_progress)
		return;

	pgd = mm->pgd;
	pgd_page = virt_to_page(pgd);
	primary_node = page_to_nid(pgd_page);

	if (!READ_ONCE(pgd_page->pt_replica))
		return;

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		pgd_t *node_pgd;
		struct page *node_pgd_page;
		int pgd_idx;

		if (!mm->repl_in_progress)
			return;

		node_pgd_page = get_replica_for_node(pgd_page, node);
		if (!node_pgd_page ||
		    page_to_nid(node_pgd_page) != node)
			continue;

		node_pgd = page_address(node_pgd_page);

		for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY;
		     pgd_idx++) {
			pgd_t pgdval;
			unsigned long child_phys;
			struct page *child_page, *local_child;
			p4d_t *node_p4d_base;

			if (!mm->repl_in_progress)
				return;

			pgdval = READ_ONCE(node_pgd[pgd_idx]);
			if (pgd_none(pgdval) || !pgd_present(pgdval))
				continue;

			child_phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (child_phys) {
				child_page = pfn_to_page(
					child_phys >> PAGE_SHIFT);
				if (READ_ONCE(child_page->pt_replica)) {
					local_child =
						get_replica_for_node(
							child_page, node);
					if (local_child &&
					    page_to_nid(local_child) == node) {
						unsigned long new_phys;
						pgdval_t new_val;

						new_phys = __pa(
							page_address(
								local_child));
						new_val = new_phys |
							(pgd_val(pgdval) &
							 ~PTE_PFN_MASK);
						WRITE_ONCE(
							node_pgd[pgd_idx],
							__pgd(new_val));
						phase2_rewire_pgd_pti(
							node_pgd, pgd_idx,
							new_phys);
					}
				}
			}

			node_p4d_base = p4d_offset(
				&node_pgd[pgd_idx], 0);
			phase2_rewire_p4d_entries(node_p4d_base, mm,
						  node, primary_node);
		}
	}

	/* Ensure all rewired pointers are visible */
	smp_mb();
}

static void replicate_existing_pagetables(struct mm_struct *mm)
{
	if (!mm || !mm->repl_in_progress)
		return;

	replicate_existing_pagetables_phase1(mm);
	replicate_existing_pagetables_phase2(mm);

	/* Full barrier after completing all replication */
	smp_mb();
}

int pgtable_repl_enable(struct mm_struct *mm, nodemask_t nodes)
{
	struct page *pgd_pages[NUMA_NODE_COUNT];
	struct page *base_page;
	pgd_t *base_pgd;
	int node, count = 0, base_node, ret = 0, i;
	struct task_struct *t;

	if (!mm || mm == &init_mm || nodes_empty(nodes) ||
	    nodes_weight(nodes) < 2)
		return -EINVAL;

	for_each_node_mask(node, nodes) {
		if (!node_online(node))
			return -EINVAL;
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++)
		WRITE_ONCE(mm->repl_steering[i], -1);

	mutex_lock(&global_repl_mutex);
	mutex_lock(&mm->repl_mutex);

	if (mm->repl_pgd_enabled) {
		ret = nodes_equal(mm->repl_pgd_nodes, nodes) ?
			0 : -EALREADY;
		goto out_unlock;
	}

	base_pgd = mm->pgd;
	base_page = virt_to_page(base_pgd);
	base_node = page_to_nid(base_page);

	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	mm->original_pgd = base_pgd;

	if (READ_ONCE(base_page->pt_replica))
		free_replica_chain_safe(base_page, "pgd",
					mitosis_pgd_alloc_order());

	WRITE_ONCE(base_page->pt_replica, NULL);

	ret = alloc_pgd_replicas(base_page, mm, nodes, pgd_pages, &count);
	if (ret)
		goto fail_cleanup;

	for (i = 1; i < count; i++) {
		pgd_t *dst_pgd = page_address(pgd_pages[i]);

		memcpy(dst_pgd, base_pgd, PAGE_SIZE);

		if (mitosis_pti_active()) {
			pgd_t *src_user;
			pgd_t *dst_user;

			src_user = mitosis_kernel_to_user_pgd(base_pgd);
			dst_user = mitosis_kernel_to_user_pgd(dst_pgd);
			if (src_user && dst_user)
				memcpy(dst_user, src_user, PAGE_SIZE);
		}
	}

	if (WARN_ON_ONCE(!link_page_replicas(pgd_pages, count))) {
		ret = -ENOMEM;
		goto fail_cleanup;
	}

	mm->repl_pgd_nodes = nodes;
	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));

	for (i = 0; i < count; i++) {
		int node_id;

		node_id = page_to_nid(pgd_pages[i]);
		mm->pgd_replicas[node_id] = page_address(pgd_pages[i]);
	}

	mmap_write_lock(mm);

	rcu_read_lock();
	for_each_thread(current, t) {
		if (t != current)
			send_sig(SIGSTOP, t, 1);
	}
	rcu_read_unlock();

	/* Publish replication state; pairs with smp_load_acquire() readers */
	smp_store_release(&mm->repl_in_progress, true);
	/* Publish enabled state; pairs with smp_load_acquire() readers */
	smp_store_release(&mm->repl_pgd_enabled, true);

	/* Ensure flags are visible before walking page tables */
	smp_mb();

	replicate_existing_pagetables(mm);

	/* Ensure all replicas are visible before clearing in_progress */
	smp_mb();
	/* Replication complete; pairs with smp_load_acquire() readers */
	smp_store_release(&mm->repl_in_progress, false);

	mmap_write_unlock(mm);

	rcu_read_lock();
	for_each_thread(current, t) {
		if (t != current)
			send_sig(SIGCONT, t, 1);
	}
	rcu_read_unlock();

	pgtable_repl_force_steering_switch(mm, NULL);

	mutex_unlock(&mm->repl_mutex);
	mutex_unlock(&global_repl_mutex);

	pr_info("MITOSIS: Enabled page table replication for mm %p on %d nodes\n",
		mm, count);
	return 0;

fail_cleanup:
	WRITE_ONCE(base_page->pt_replica, NULL);
	mm->repl_pgd_enabled = false;
	mm->repl_in_progress = false;
	nodes_clear(mm->repl_pgd_nodes);
	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));
	mm->original_pgd = NULL;

out_unlock:
	mutex_unlock(&mm->repl_mutex);
	mutex_unlock(&global_repl_mutex);
	return ret;
}
EXPORT_SYMBOL(pgtable_repl_enable);

static void switch_cr3_ipi(void *info)
{
	struct cr3_switch_info *switch_info = info;
	struct mm_struct *mm;
	pgd_t *original_pgd;
	unsigned long original_pgd_pa, current_cr3, current_pgd_pa;

	if (!switch_info || !switch_info->mm || !switch_info->original_pgd)
		return;

	mm = switch_info->mm;
	original_pgd = switch_info->original_pgd;

	if (current->mm != mm && current->active_mm != mm)
		return;

	original_pgd_pa = __pa(original_pgd);
	current_cr3 = __read_cr3();
	current_pgd_pa = current_cr3 & PAGE_MASK;

	if (current_pgd_pa != original_pgd_pa) {
		unsigned long new_cr3;

		new_cr3 = original_pgd_pa | (current_cr3 & ~PAGE_MASK);
		native_write_cr3(new_cr3);
		__flush_tlb_all();
	}
}

static void free_all_replicas_via_chains(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;
	unsigned long child_phys;

	if (!mm)
		return;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			child_phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (child_phys)
				free_replica_chain_safe(
					pfn_to_page(child_phys >> PAGE_SHIFT),
					"p4d", 0);
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			pud_t *pud_base;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			child_phys = p4d_val(p4dval) & PTE_PFN_MASK;
			if (child_phys)
				free_replica_chain_safe(
					pfn_to_page(child_phys >> PAGE_SHIFT),
					"pud", 0);

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD;
			     pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) ||
				    !pud_present(pudval) ||
				    pud_trans_huge(pudval))
					continue;

				child_phys = pud_val(pudval) & PTE_PFN_MASK;
				if (child_phys)
					free_replica_chain_safe(
						pfn_to_page(
							child_phys >>
							PAGE_SHIFT),
						"pmd", 0);

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD;
				     pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(
						pmd_base[pmd_idx]);
					if (pmd_none(pmdval) ||
					    !pmd_present(pmdval) ||
					    pmd_trans_huge(pmdval))
						continue;

					child_phys = pmd_val(pmdval) &
						     PTE_PFN_MASK;
					if (child_phys)
						free_replica_chain_safe(
							pfn_to_page(
								child_phys >>
								PAGE_SHIFT),
							"pte", 0);
				}
			}
		}
	}

	/* Ensure all frees complete before returning */
	smp_mb();
}

static void free_pgd_replicas(struct mm_struct *mm, int keep_node)
{
	struct page *primary_pgd_page;
	int node;
	int alloc_order = mitosis_pgd_alloc_order();

	if (!mm || !mm->pgd)
		return;

	primary_pgd_page = virt_to_page(mm->pgd);
	WRITE_ONCE(primary_pgd_page->pt_replica, NULL);

	/* Ensure primary replica pointer is cleared before freeing others */
	smp_wmb();

	for_each_node_mask(node, mm->repl_pgd_nodes) {
		pgd_t *replica_pgd;
		struct page *replica_page;
		bool from_cache;

		if (node == keep_node)
			continue;

		replica_pgd = mm->pgd_replicas[node];
		if (!replica_pgd)
			continue;

		replica_page = virt_to_page(replica_pgd);
		from_cache = PageMitosisFromCache(replica_page);
		WRITE_ONCE(replica_page->pt_replica, NULL);

		replica_page->pt_owner_mm = NULL;

		if (alloc_order == 0 && from_cache) {
			ClearPageMitosisFromCache(replica_page);
			replica_page->pt_replica = NULL;
			if (mitosis_cache_push(replica_page, node,
					       MITOSIS_CACHE_PGD)) {
				mm->pgd_replicas[node] = NULL;
				continue;
			}
		}

		ClearPageMitosisFromCache(replica_page);
		__free_pages(replica_page, alloc_order);

		mm->pgd_replicas[node] = NULL;
	}
}

void pgtable_repl_disable(struct mm_struct *mm)
{
	unsigned long flags;
	int original_node;
	struct cr3_switch_info switch_info;

	if (!mm || mm == &init_mm)
		return;

	mutex_lock(&global_repl_mutex);

	if (!mm->repl_pgd_enabled && nodes_empty(mm->repl_pgd_nodes)) {
		mutex_unlock(&global_repl_mutex);
		return;
	}

	mutex_lock(&mm->repl_mutex);

	if (!mm->original_pgd)
		mm->original_pgd = mm->pgd;

	original_node = page_to_nid(virt_to_page(mm->original_pgd));

	/* Publish disabled state; pairs with smp_load_acquire() readers */
	smp_store_release(&mm->repl_pgd_enabled, false);

	/* Ensure disabled flag is visible before switching CR3 */
	smp_mb();

	WRITE_ONCE(mm->pgd, mm->original_pgd);

	/* Ensure pgd write is visible before IPI */
	smp_mb();

	switch_info.mm = mm;
	switch_info.original_pgd = mm->original_pgd;
	switch_info.initiating_cpu = smp_processor_id();

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm) {
		unsigned long current_cr3_pa = __read_cr3() & PAGE_MASK;
		unsigned long original_pgd_pa;

		original_pgd_pa = __pa(mm->original_pgd);
		if (current_cr3_pa != original_pgd_pa) {
			native_write_cr3(original_pgd_pa |
					 (__read_cr3() & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
	local_irq_restore(flags);

	on_each_cpu_mask(mm_cpumask(mm), switch_cr3_ipi, &switch_info, 1);

	/* Ensure all CPUs have switched CR3 before freeing replicas */
	smp_mb();
	synchronize_rcu();

	free_all_replicas_via_chains(mm);
	free_pgd_replicas(mm, original_node);

	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));
	nodes_clear(mm->repl_pgd_nodes);
	mm->original_pgd = NULL;

	pr_info("MITOSIS: Disabled page table replication for mm %p\n", mm);

	mutex_unlock(&mm->repl_mutex);
	mutex_unlock(&global_repl_mutex);
	synchronize_rcu();
}
EXPORT_SYMBOL(pgtable_repl_disable);

static int __init mitosis_setup(char *str)
{
	sysctl_mitosis_mode = 1;
	return 1;
}
__setup("mitosis", mitosis_setup);

int mitosis_sysctl_handler(const struct ctl_table *table, int write,
			   void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	int new_val;
	const struct ctl_table tmp_table = {
		.data = &new_val,
		.maxlen = sizeof(int),
		.mode = table->mode,
	};

	new_val = sysctl_mitosis_mode;

	ret = proc_dointvec(&tmp_table, write, buffer, lenp, ppos);
	if (ret < 0)
		return ret;

	if (write) {
		if (new_val > 1)
			new_val = 1;
		else if (new_val < -1)
			new_val = -1;

		sysctl_mitosis_mode = new_val;

		if (new_val == 1)
			pr_info("Mitosis: mode=1 (replication auto-enabled for new processes)\n");
		else if (new_val == 0)
			pr_info("Mitosis: mode=0 (all page table allocations forced to node 0)\n");
		else
			pr_info("Mitosis: mode=-1 (default allocation, no special handling)\n");
	}

	return 0;
}
EXPORT_SYMBOL(mitosis_sysctl_handler);

int mitosis_inherit_sysctl_handler(const struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	const struct ctl_table tmp_table = {
		.data = &sysctl_mitosis_inherit,
		.maxlen = sizeof(int),
		.mode = table->mode,
	};

	ret = proc_dointvec_minmax(&tmp_table, write, buffer, lenp, ppos);
	if (ret < 0)
		return ret;

	if (write) {
		if (sysctl_mitosis_inherit <= 0)
			sysctl_mitosis_inherit = -1;
		else
			sysctl_mitosis_inherit = 1;

		pr_info("Mitosis: Inheritance for child processes set to %s.\n",
			sysctl_mitosis_inherit == 1 ? "ENABLED" : "DISABLED");
	}

	return 0;
}
EXPORT_SYMBOL(mitosis_inherit_sysctl_handler);

struct mitosis_enable_work {
	struct callback_head twork;
	nodemask_t nodes;
	int result;
	struct completion done;
};

static void mitosis_enable_task_work_fn(struct callback_head *head)
{
	struct mitosis_enable_work *work =
		container_of(head, struct mitosis_enable_work, twork);

	work->result = pgtable_repl_enable(current->mm, work->nodes);
	complete(&work->done);
}

int pgtable_repl_enable_external(struct task_struct *target, nodemask_t nodes)
{
	struct mitosis_enable_work work;
	int ret;

	if (target == current)
		return pgtable_repl_enable(current->mm, nodes);

	if (!target->mm)
		return -EINVAL;

	init_completion(&work.done);
	work.nodes = nodes;
	work.result = -EINVAL;
	init_task_work(&work.twork, mitosis_enable_task_work_fn);

	ret = task_work_add(target, &work.twork, TWA_SIGNAL);
	if (ret)
		return ret;

	wait_for_completion(&work.done);

	return work.result;
}
EXPORT_SYMBOL(pgtable_repl_enable_external);

struct mitosis_disable_work {
	struct callback_head twork;
	int result;
	struct completion done;
};

static void mitosis_disable_task_work_fn(struct callback_head *head)
{
	struct mitosis_disable_work *work =
		container_of(head, struct mitosis_disable_work, twork);

	pgtable_repl_disable(current->mm);
	work->result = 0;
	complete(&work->done);
}

int pgtable_repl_disable_external(struct task_struct *target)
{
	struct mitosis_disable_work work;
	int ret;

	if (target == current) {
		pgtable_repl_disable(current->mm);
		return 0;
	}

	if (!target->mm)
		return -EINVAL;

	init_completion(&work.done);
	work.result = -EINVAL;
	init_task_work(&work.twork, mitosis_disable_task_work_fn);

	ret = task_work_add(target, &work.twork, TWA_SIGNAL);
	if (ret)
		return ret;

	wait_for_completion(&work.done);
	return work.result;
}
EXPORT_SYMBOL(pgtable_repl_disable_external);

static int __init mitosis_check_numa_node_count(void)
{
	int online = num_online_nodes();

	if (online != NUMA_NODE_COUNT) {
		pr_emerg("MITOSIS: CONFIG_MITOSIS_NUMA_NODE_COUNT=%d but system has %d NUMA nodes.\n",
			 NUMA_NODE_COUNT, online);
		pr_emerg("MITOSIS: Reconfigure kernel with CONFIG_MITOSIS_NUMA_NODE_COUNT=%d\n",
			 online);
		pr_emerg("MITOSIS: Check node count with: numactl --hardware\n");
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	pr_info("MITOSIS: NUMA node count matches: %d nodes\n", online);
	return 0;
}
early_initcall(mitosis_check_numa_node_count);
