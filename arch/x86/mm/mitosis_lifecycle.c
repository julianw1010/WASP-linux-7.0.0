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
#include <linux/sched/mm.h>
#include <linux/mempolicy.h>

int sysctl_mitosis_inherit = 1;

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
		pages[i]->pt_replica = NULL;

	for (i = 0; i < count - 1; i++)
		WRITE_ONCE(pages[i]->pt_replica, pages[i + 1]);
	WRITE_ONCE(pages[count - 1]->pt_replica, pages[0]);

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

	start_page = base;
	page = base->pt_replica;

	while (page && page != start_page) {
		if (page_to_nid(page) == target_node)
			return page;
		page = page->pt_replica;
	}

	return NULL;
}

static void replicate_existing_pagetables_phase1(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	if (!mm || !mm->repl_in_progress)
		return;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;

		if (!mm->repl_in_progress)
			return;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			unsigned long child_phys = pgd_val(pgdval) & PTE_PFN_MASK;

			if (child_phys) {
				struct page *child_page = pfn_to_page(child_phys >> PAGE_SHIFT);

				if (mm->repl_in_progress && !child_page->pt_replica) {
					struct page *pages[NUMA_NODE_COUNT];
					int count = 0;
					int i, ret;
					void *src;

					ret = alloc_p4d_replicas(child_page, mm, pages, &count);
					if (ret == 0 && count >= 2) {
						src = page_address(child_page);
						for (i = 1; i < count; i++)
							memcpy(page_address(pages[i]), src, PAGE_SIZE);
						BUG_ON(!link_page_replicas(pages, count));
					}
				}
			}
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			pud_t *pud_base;

			if (!mm->repl_in_progress)
				return;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			{
				unsigned long pud_phys = p4d_val(p4dval) & PTE_PFN_MASK;

				if (pud_phys) {
					struct page *pud_page = pfn_to_page(pud_phys >> PAGE_SHIFT);

					if (mm->repl_in_progress && !pud_page->pt_replica) {
						struct page *pages[NUMA_NODE_COUNT];
						int count = 0;
						int i, ret;
						void *src;

						ret = alloc_pud_replicas(pud_page, mm, pages, &count);
						if (ret == 0 && count >= 2) {
							src = page_address(pud_page);
							for (i = 1; i < count; i++)
								memcpy(page_address(pages[i]), src, PAGE_SIZE);
							BUG_ON(!link_page_replicas(pages, count));
						}
					}
				}
			}

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				if (!mm->repl_in_progress)
					return;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
					continue;

				{
					unsigned long pmd_phys = pud_val(pudval) & PTE_PFN_MASK;

					if (pmd_phys) {
						struct page *pmd_page = pfn_to_page(pmd_phys >> PAGE_SHIFT);

						if (mm->repl_in_progress && !pmd_page->pt_replica) {
							struct page *pages[NUMA_NODE_COUNT];
							int count = 0;
							int i, ret;
							void *src;

							ret = alloc_pmd_replicas(pmd_page, mm, pages, &count);
							if (ret == 0 && count >= 2) {
								src = page_address(pmd_page);
								for (i = 1; i < count; i++)
									memcpy(page_address(pages[i]), src, PAGE_SIZE);
								BUG_ON(!link_page_replicas(pages, count));
							}
						}
					}
				}

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					if (!mm->repl_in_progress)
						return;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
						continue;

					{
						unsigned long pte_phys = pmd_val(pmdval) & PTE_PFN_MASK;

						if (pte_phys) {
							struct page *pte_page = pfn_to_page(pte_phys >> PAGE_SHIFT);

							if (mm->repl_in_progress && !pte_page->pt_replica) {
								struct page *pages[NUMA_NODE_COUNT];
								int count = 0;
								int i, ret;
								void *src;

								ret = alloc_pte_replicas(pte_page, mm, pages, &count);
								if (ret == 0 && count >= 2) {
									src = page_address(pte_page);
									for (i = 1; i < count; i++)
										memcpy(page_address(pages[i]), src, PAGE_SIZE);
									BUG_ON(!link_page_replicas(pages, count));
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

	if (!pgd_page->pt_replica)
		return;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *node_pgd;
		struct page *node_pgd_page;
		int pgd_idx;

		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		if (!mm->repl_in_progress)
			return;

		node_pgd_page = get_replica_for_node(pgd_page, node);
		if (!node_pgd_page || page_to_nid(node_pgd_page) != node)
			continue;

		node_pgd = page_address(node_pgd_page);

		for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
			pgd_t pgdval;
			p4d_t *node_p4d_base;
			unsigned long child_phys;
			struct page *child_page;
			int p4d_idx;

			if (!mm->repl_in_progress)
				return;

			pgdval = READ_ONCE(node_pgd[pgd_idx]);
			if (pgd_none(pgdval) || !pgd_present(pgdval))
				continue;

			child_phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (child_phys) {
				child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
				if (child_page->pt_replica) {
					struct page *local_child = get_replica_for_node(child_page, node);

					if (local_child && page_to_nid(local_child) == node) {
						unsigned long new_phys = __pa(page_address(local_child));
						pgdval_t new_val = new_phys | (pgd_val(pgdval) & ~PTE_PFN_MASK);

						WRITE_ONCE(node_pgd[pgd_idx], __pgd(new_val));

						if (mitosis_pti_active()) {
							pgd_t *user_entry = mitosis_get_user_pgd_entry(&node_pgd[pgd_idx]);

							if (user_entry) {
								pgdval_t user_flags = pgd_val(*user_entry) & ~PTE_PFN_MASK;

								WRITE_ONCE(*user_entry, __pgd(new_phys | user_flags));
							}
						}
					}
				}
			}

			node_p4d_base = p4d_offset(&node_pgd[pgd_idx], 0);

			for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
				p4d_t p4dval;
				pud_t *node_pud_base;
				int pud_idx;

				if (!mm->repl_in_progress)
					return;

				p4dval = READ_ONCE(node_p4d_base[p4d_idx]);
				if (p4d_none(p4dval) || !p4d_present(p4dval))
					continue;

				child_phys = p4d_val(p4dval) & PTE_PFN_MASK;
				if (child_phys) {
					child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
					if (child_page->pt_replica) {
						struct page *local_child = get_replica_for_node(child_page, node);

						if (local_child && page_to_nid(local_child) == node) {
							unsigned long new_phys = __pa(page_address(local_child));
							p4dval_t new_val = new_phys | (p4d_val(p4dval) & ~PTE_PFN_MASK);

							WRITE_ONCE(node_p4d_base[p4d_idx], __p4d(new_val));
						}
					}
				}

				node_pud_base = pud_offset(&node_p4d_base[p4d_idx], 0);

				for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
					pud_t pudval;
					pmd_t *node_pmd_base;
					int pmd_idx;

					if (!mm->repl_in_progress)
						return;

					pudval = READ_ONCE(node_pud_base[pud_idx]);
					if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
						continue;

					child_phys = pud_val(pudval) & PTE_PFN_MASK;
					if (child_phys) {
						child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
						if (child_page->pt_replica) {
							struct page *local_child = get_replica_for_node(child_page, node);

							if (local_child && page_to_nid(local_child) == node) {
								unsigned long new_phys = __pa(page_address(local_child));
								pudval_t new_val = new_phys | (pud_val(pudval) & ~PTE_PFN_MASK);

								if (node == primary_node &&
								    local_child != child_page) {
									struct ptdesc *orig_ptdesc = page_ptdesc(child_page);
									struct ptdesc *local_ptdesc = page_ptdesc(local_child);

									if (orig_ptdesc->pmd_huge_pte &&
									    !local_ptdesc->pmd_huge_pte) {
										local_ptdesc->pmd_huge_pte = orig_ptdesc->pmd_huge_pte;
										orig_ptdesc->pmd_huge_pte = NULL;
									}
								}

								WRITE_ONCE(node_pud_base[pud_idx], __pud(new_val));
							}
						}
					}

					node_pmd_base = pmd_offset(&node_pud_base[pud_idx], 0);

					for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
						pmd_t pmdval;

						if (!mm->repl_in_progress)
							return;

						pmdval = READ_ONCE(node_pmd_base[pmd_idx]);
						if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
							continue;

						child_phys = pmd_val(pmdval) & PTE_PFN_MASK;
						if (child_phys) {
							child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
							if (child_page->pt_replica) {
								struct page *local_child = get_replica_for_node(child_page, node);

								if (local_child && page_to_nid(local_child) == node) {
									unsigned long new_phys = __pa(page_address(local_child));
									pmdval_t new_val = new_phys | (pmd_val(pmdval) & ~PTE_PFN_MASK);

									WRITE_ONCE(node_pmd_base[pmd_idx], __pmd(new_val));
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

int pgtable_repl_enable(struct mm_struct *mm)
{
	struct page *pgd_pages[NUMA_NODE_COUNT];
	struct page *base_page;
	pgd_t *base_pgd;
	nodemask_t nodes;
	int count = 0, base_node, ret = 0, i;

	if (!mm || mm == &init_mm)
		return -EINVAL;

	nodes = node_online_map;

	if (nodes_weight(nodes) < 2)
		return -EINVAL;

	for (i = 0; i < NUMA_NODE_COUNT; i++)
		mm->repl_steering[i] = -1;

	mutex_lock(&mm->repl_mutex);

	if (mm->repl_pgd_enabled) {
		ret = 0;
		goto out_unlock;
	}

	base_pgd = mm->pgd;
	base_page = virt_to_page(base_pgd);
	base_node = page_to_nid(base_page);

	if (!node_isset(base_node, nodes))
		node_set(base_node, nodes);

	mm->original_pgd = base_pgd;

	if (base_page->pt_replica)
		mitosis_free_replica_chain(base_page, MITOSIS_CACHE_PGD, mitosis_pgd_alloc_order());

	base_page->pt_replica = NULL;

	ret = alloc_pgd_replicas(base_page, mm, pgd_pages, &count);
	BUG_ON(ret);

	for (i = 1; i < count; i++) {
		pgd_t *dst_pgd = page_address(pgd_pages[i]);

		memcpy(dst_pgd, base_pgd, PAGE_SIZE);

		if (mitosis_pti_active()) {
			pgd_t *src_user = mitosis_kernel_to_user_pgd(base_pgd);
			pgd_t *dst_user = mitosis_kernel_to_user_pgd(dst_pgd);

			if (src_user && dst_user)
				memcpy(dst_user, src_user, PAGE_SIZE);
		}
	}

	BUG_ON(!link_page_replicas(pgd_pages, count));
	mm->repl_pgd_nodes = nodes;
	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));

	for (i = 0; i < count; i++) {
		int node_id = page_to_nid(pgd_pages[i]);

		mm->pgd_replicas[node_id] = page_address(pgd_pages[i]);
	}

	mmap_write_lock(mm);

	smp_store_release(&mm->repl_in_progress, true);
	smp_store_release(&mm->repl_pgd_enabled, true);

	replicate_existing_pagetables_phase1(mm);
	replicate_existing_pagetables_phase2(mm);

	smp_store_release(&mm->repl_in_progress, false);

	mmap_write_unlock(mm);

	pgtable_repl_force_steering_switch(mm, NULL);

	mutex_unlock(&mm->repl_mutex);

	pr_info("MITOSIS: Enabled page table replication for mm %px on %d nodes\n", mm, count);
	return 0;

out_unlock:
	mutex_unlock(&mm->repl_mutex);
	return ret;
}

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
		unsigned long new_cr3 = original_pgd_pa | (current_cr3 & ~PAGE_MASK);

		native_write_cr3(new_cr3);
		__flush_tlb_all();
	}
}

void pgtable_repl_disable(struct mm_struct *mm)
{
	unsigned long flags;
	int original_node;
	struct cr3_switch_info switch_info;
	struct page *primary_pgd_page;
	int alloc_order;
	int node;
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	if (!mm || mm == &init_mm)
		return;

	mutex_lock(&mm->repl_mutex);

	if (!mm->repl_pgd_enabled && nodes_empty(mm->repl_pgd_nodes)) {
		mutex_unlock(&mm->repl_mutex);
		return;
	}

	if (!mm->original_pgd)
		mm->original_pgd = mm->pgd;

	original_node = page_to_nid(virt_to_page(mm->original_pgd));

	smp_store_release(&mm->repl_pgd_enabled, false);

	WRITE_ONCE(mm->pgd, mm->original_pgd);

	switch_info.mm = mm;
	switch_info.original_pgd = mm->original_pgd;
	switch_info.initiating_cpu = smp_processor_id();

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm) {
		unsigned long current_cr3_pa = __read_cr3() & PAGE_MASK;
		unsigned long original_pgd_pa = __pa(mm->original_pgd);

		if (current_cr3_pa != original_pgd_pa) {
			native_write_cr3(original_pgd_pa | (__read_cr3() & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
	local_irq_restore(flags);

	on_each_cpu_mask(mm_cpumask(mm), switch_cr3_ipi, &switch_info, 1);

	synchronize_rcu();

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;
		unsigned long child_phys;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled()) {
			child_phys = pgd_val(pgdval) & PTE_PFN_MASK;
			if (child_phys)
				mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_P4D, 0);
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
				mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PUD, 0);

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
					continue;

				child_phys = pud_val(pudval) & PTE_PFN_MASK;
				if (child_phys)
					mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PMD, 0);

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
						continue;

					child_phys = pmd_val(pmdval) & PTE_PFN_MASK;
					if (child_phys)
						mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PTE, 0);
				}
			}
		}
	}

	primary_pgd_page = virt_to_page(mm->pgd);
	alloc_order = mitosis_pgd_alloc_order();

	primary_pgd_page->pt_replica = NULL;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *replica_pgd;
		struct page *replica_page;
		bool from_cache;

		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		if (node == original_node)
			continue;

		replica_pgd = mm->pgd_replicas[node];
		if (!replica_pgd)
			continue;

		replica_page = virt_to_page(replica_pgd);
		from_cache = PageMitosisFromCache(replica_page);
		replica_page->pt_replica = NULL;

		replica_page->pt_owner_mm = NULL;

		if (alloc_order == 0 && from_cache) {
			ClearPageMitosisFromCache(replica_page);
			replica_page->pt_replica = NULL;
			if (mitosis_cache_push(replica_page, node, MITOSIS_CACHE_PGD)) {
				mm->pgd_replicas[node] = NULL;
				continue;
			}
		}

		ClearPageMitosisFromCache(replica_page);
		__free_pages(replica_page, alloc_order);

		mm->pgd_replicas[node] = NULL;
	}

	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));
	nodes_clear(mm->repl_pgd_nodes);
	mm->original_pgd = NULL;

	pr_info("MITOSIS: Disabled page table replication for mm %p\n", mm);
	mutex_unlock(&mm->repl_mutex);
}

int mitosis_inherit_sysctl_handler(struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;
	struct ctl_table tmp_table = {
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

struct mitosis_enable_work {
	struct callback_head twork;
	int result;
	struct completion done;
};

static void mitosis_enable_task_work_fn(struct callback_head *head)
{
	struct mitosis_enable_work *work =
		container_of(head, struct mitosis_enable_work, twork);

	work->result = pgtable_repl_enable(current->mm);
	complete(&work->done);
}

int pgtable_repl_enable_external(struct task_struct *target)
{
	struct mitosis_enable_work work;
	int ret;

	if (target == current)
		return pgtable_repl_enable(current->mm);

	if (!target->mm)
		return -EINVAL;

	init_completion(&work.done);
	work.result = -EINVAL;
	init_task_work(&work.twork, mitosis_enable_task_work_fn);

	ret = task_work_add(target, &work.twork, TWA_SIGNAL);
	if (ret)
		return ret;

	wait_for_completion(&work.done);

	return work.result;
}

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

static int __init mitosis_check_numa_node_count(void)
{
	int online = num_online_nodes();

	if (online != NUMA_NODE_COUNT) {
		pr_emerg("MITOSIS: CONFIG_MITOSIS_NUMA_NODE_COUNT=%d but system has %d NUMA nodes.\n",
			 NUMA_NODE_COUNT, online);
		pr_emerg("MITOSIS: Reconfigure kernel with CONFIG_MITOSIS_NUMA_NODE_COUNT=%d\n",
			 online);
		pr_emerg("MITOSIS: Check node count with: numactl --hardware\n");
		BUG();
	}

	pr_info("MITOSIS: NUMA node count matches: %d nodes\n", online);
	return 0;
}
early_initcall(mitosis_check_numa_node_count);

