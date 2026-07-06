#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/mitosis.h>
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
#include <linux/mitosis_stats.h>

int sysctl_mitosis_inherit = 1;

struct cr3_switch_info {
	struct mm_struct *mm;
	int initiating_cpu;
};

bool mitosis_link_page_replicas(struct page **pages, int count)
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

struct page *mitosis_get_replica_for_node(struct page *base, int target_node)
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

typedef int (*alloc_replicas_fn_t)(struct page *, struct mm_struct *,
				   struct page **, int *);

static void replicate_entry_page(unsigned long entry_phys, struct mm_struct *mm,
				 alloc_replicas_fn_t alloc_fn, spinlock_t *ptl)
{
	struct page *child_page;
	struct page *pages[NUMA_NODE_COUNT];
	int count = 0;
	int i, ret;
	void *src;

	if (!entry_phys)
		return;

	child_page = pfn_to_page(entry_phys >> PAGE_SHIFT);
	if (child_page->pt_replica)
		return;

	ret = alloc_fn(child_page, mm, pages, &count);
	if (ret == 0 && count >= 2) {
		if (ptl)
			spin_lock(ptl);
		src = page_address(child_page);
		for (i = 1; i < count; i++)
			memcpy(page_address(pages[i]), src, PAGE_SIZE);
		mitosis_link_page_replicas(pages, count);
		if (ptl)
			spin_unlock(ptl);
	}
}

static void replicate_existing_pagetables_phase1(struct mm_struct *mm)
{
	pgd_t *pgd;
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	if (!mm)
		return;

	pgd = mm->pgd;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval;
		p4d_t *p4d_base;

		pgdval = READ_ONCE(pgd[pgd_idx]);
		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled())
			replicate_entry_page(pgd_val(pgdval) & PTE_PFN_MASK,
					     mm, mitosis_alloc_p4d_replicas, NULL);

		p4d_base = p4d_offset(&pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval;
			pud_t *pud_base;

			p4dval = READ_ONCE(p4d_base[p4d_idx]);
			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			replicate_entry_page(p4d_val(p4dval) & PTE_PFN_MASK,
					     mm, mitosis_alloc_pud_replicas,
					     pud_lockptr(mm, pud_base));

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
					continue;

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				replicate_entry_page(pud_val(pudval) & PTE_PFN_MASK,
						     mm, mitosis_alloc_pmd_replicas,
						     pmd_lockptr(mm, pmd_base));

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
						continue;

					replicate_entry_page(pmd_val(pmdval) & PTE_PFN_MASK,
							     mm, mitosis_alloc_pte_replicas,
							     pte_lockptr(mm, &pmd_base[pmd_idx]));
				}
			}
		}
	}

	smp_mb();
}

static struct page *find_local_replica_for_rewrite(unsigned long entry_val,
						   int node,
						   struct page **orig_page_out)
{
	unsigned long child_phys;
	struct page *child_page, *local_child;

	child_phys = entry_val & PTE_PFN_MASK;
	if (!child_phys)
		return NULL;

	child_page = pfn_to_page(child_phys >> PAGE_SHIFT);
	if (!child_page->pt_replica)
		return NULL;

	local_child = mitosis_get_replica_for_node(child_page, node);
	if (!local_child || page_to_nid(local_child) != node)
		return NULL;

	if (orig_page_out)
		*orig_page_out = child_page;
	return local_child;
}

static void replicate_existing_pagetables_phase2(struct mm_struct *mm)
{
	pgd_t *pgd;
	struct page *pgd_page;
	int node;
	int primary_node;

	if (!mm)
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

		node_pgd_page = mitosis_get_replica_for_node(pgd_page, node);
		if (!node_pgd_page || page_to_nid(node_pgd_page) != node)
			continue;

		node_pgd = page_address(node_pgd_page);

		for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
			pgd_t pgdval;
			p4d_t *node_p4d_base;
			struct page *local_child;
			int p4d_idx;

			pgdval = READ_ONCE(node_pgd[pgd_idx]);
			if (pgd_none(pgdval) || !pgd_present(pgdval))
				continue;

			local_child = find_local_replica_for_rewrite(
				pgd_val(pgdval), node, NULL);
			if (local_child) {
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

			node_p4d_base = p4d_offset(&node_pgd[pgd_idx], 0);

			for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
				p4d_t p4dval;
				pud_t *node_pud_base;
				int pud_idx;

				p4dval = READ_ONCE(node_p4d_base[p4d_idx]);
				if (p4d_none(p4dval) || !p4d_present(p4dval))
					continue;

				local_child = find_local_replica_for_rewrite(
					p4d_val(p4dval), node, NULL);
				if (local_child) {
					unsigned long new_phys = __pa(page_address(local_child));
					p4dval_t new_val = new_phys | (p4d_val(p4dval) & ~PTE_PFN_MASK);

					WRITE_ONCE(node_p4d_base[p4d_idx], __p4d(new_val));
				}

				node_pud_base = pud_offset(&node_p4d_base[p4d_idx], 0);

				for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
					pud_t pudval;
					pmd_t *node_pmd_base;
					int pmd_idx;

					pudval = READ_ONCE(node_pud_base[pud_idx]);
					if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
						continue;

					{
						struct page *orig_page = NULL;

						local_child = find_local_replica_for_rewrite(
							pud_val(pudval), node, &orig_page);
						if (local_child) {
							unsigned long new_phys = __pa(page_address(local_child));
							pudval_t new_val = new_phys | (pud_val(pudval) & ~PTE_PFN_MASK);

							if (READ_ONCE(mitosis_verify))
								BUG_ON(node == primary_node &&
								       local_child != orig_page);

							WRITE_ONCE(node_pud_base[pud_idx], __pud(new_val));
						}
					}

					node_pmd_base = pmd_offset(&node_pud_base[pud_idx], 0);

					for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
						pmd_t pmdval;

						pmdval = READ_ONCE(node_pmd_base[pmd_idx]);
						if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
							continue;

						local_child = find_local_replica_for_rewrite(
							pmd_val(pmdval), node, NULL);
						if (local_child) {
							unsigned long new_phys = __pa(page_address(local_child));
							pmdval_t new_val = new_phys | (pmd_val(pmdval) & ~PTE_PFN_MASK);

							WRITE_ONCE(node_pmd_base[pmd_idx], __pmd(new_val));
						}
					}
				}
			}
		}
	}

	smp_mb();
}

int mitosis_enable(struct mm_struct *mm)
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

	static_branch_enable(&mitosis_repl_ever_enabled);

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

	if (base_page->pt_replica)
		mitosis_free_replica_chain(base_page, MITOSIS_CACHE_PGD, mitosis_pgd_alloc_order());

	base_page->pt_replica = NULL;

	ret = mitosis_alloc_pgd_replicas(base_page, mm, pgd_pages, &count);

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

	mm->repl_pgd_nodes = nodes;
	memset(mm->pgd_replicas, 0, sizeof(mm->pgd_replicas));

	mmap_write_lock(mm);
	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma)
			vma_start_write(vma);
	}

	mitosis_link_page_replicas(pgd_pages, count);

	smp_store_release(&mm->repl_pgd_enabled, true);

	mitosis_stats_attach(mm, base_node);
	for (i = 1; i < count; i++)
		mitosis_pt_account_page(pgd_pages[i], MITOSIS_CACHE_PGD, 1);

	replicate_existing_pagetables_phase1(mm);
	replicate_existing_pagetables_phase2(mm);

	for (i = 0; i < count; i++) {
		int node_id = page_to_nid(pgd_pages[i]);

		smp_store_release(&mm->pgd_replicas[node_id], page_address(pgd_pages[i]));
	}
	smp_mb();

	mmap_write_unlock(mm);

	mitosis_force_steering_switch(mm, NULL);

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
	unsigned long pgd_pa, current_cr3, current_pgd_pa;

	if (!switch_info || !switch_info->mm)
		return;

	mm = switch_info->mm;

	if (current->mm != mm && current->active_mm != mm)
		return;

	pgd_pa = __pa(mm->pgd);
	current_cr3 = __read_cr3();
	current_pgd_pa = current_cr3 & PAGE_MASK;

	if (current_pgd_pa != pgd_pa) {
		unsigned long new_cr3 = pgd_pa | (current_cr3 & ~PAGE_MASK);

		native_write_cr3(new_cr3);
		__flush_tlb_all();
	}
}

void mitosis_disable(struct mm_struct *mm)
{
	unsigned long flags;
	int pgd_node;
	struct cr3_switch_info switch_info;
	struct page *pgd_page;
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

	pgd_node = page_to_nid(virt_to_page(mm->pgd));

	mmap_write_lock(mm);
	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma)
			vma_start_write(vma);
	}

	smp_store_release(&mm->repl_pgd_enabled, false);

	smp_mb();

	switch_info.mm = mm;
	switch_info.initiating_cpu = smp_processor_id();

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm) {
		unsigned long current_cr3_pa = __read_cr3() & PAGE_MASK;
		unsigned long pgd_pa = __pa(mm->pgd);

		if (current_cr3_pa != pgd_pa) {
			native_write_cr3(pgd_pa | (__read_cr3() & ~PAGE_MASK));
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

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			child_phys = p4d_val(p4dval) & PTE_PFN_MASK;
			if (child_phys) {
				spinlock_t *ptl = pud_lock(mm, pud_base);

				mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PUD, 0);
				spin_unlock(ptl);
			}

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval;
				pmd_t *pmd_base;

				pudval = READ_ONCE(pud_base[pud_idx]);
				if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
					continue;

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				child_phys = pud_val(pudval) & PTE_PFN_MASK;
				if (child_phys) {
					spinlock_t *ptl = pmd_lock(mm, pmd_base);

					mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PMD, 0);
					spin_unlock(ptl);
				}

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval;

					pmdval = READ_ONCE(pmd_base[pmd_idx]);
					if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
						continue;

					child_phys = pmd_val(pmdval) & PTE_PFN_MASK;
					if (child_phys) {
						spinlock_t *ptl = pte_lockptr(mm, &pmd_base[pmd_idx]);

						spin_lock(ptl);
						mitosis_free_replica_chain(pfn_to_page(child_phys >> PAGE_SHIFT), MITOSIS_CACHE_PTE, 0);
						spin_unlock(ptl);
					}
				}
			}
		}
	}

	pgd_page = virt_to_page(mm->pgd);
	alloc_order = mitosis_pgd_alloc_order();

	pgd_page->pt_replica = NULL;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *replica_pgd;
		struct page *replica_page;
		bool from_cache;

		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		if (node == pgd_node)
			continue;

		replica_pgd = mm->pgd_replicas[node];
		if (!replica_pgd)
			continue;

		mitosis_replica_free_inc(MITOSIS_CACHE_PGD);

		replica_page = virt_to_page(replica_pgd);
		from_cache = PageMitosisFromCache(replica_page);
		replica_page->pt_replica = NULL;

		mitosis_pt_account_page(replica_page, MITOSIS_CACHE_PGD, -1);
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

	mmap_write_unlock(mm);

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

struct mitosis_task_work {
	struct callback_head twork;
	int result;
	struct completion done;
};

static void mitosis_enable_task_work_fn(struct callback_head *head)
{
	struct mitosis_task_work *work =
		container_of(head, struct mitosis_task_work, twork);

	work->result = mitosis_enable(current->mm);
	complete(&work->done);
}

static void mitosis_disable_task_work_fn(struct callback_head *head)
{
	struct mitosis_task_work *work =
		container_of(head, struct mitosis_task_work, twork);

	mitosis_disable(current->mm);
	work->result = 0;
	complete(&work->done);
}

static int mitosis_run_on_target(struct task_struct *target,
				 void (*fn)(struct callback_head *))
{
	struct mitosis_task_work work;
	int ret;

	if (!target->mm)
		return -EINVAL;

	init_completion(&work.done);
	work.result = -EINVAL;
	init_task_work(&work.twork, fn);

	ret = task_work_add(target, &work.twork, TWA_SIGNAL);
	if (ret)
		return ret;

	if (wait_for_completion_killable(&work.done)) {
		if (task_work_cancel(target, &work.twork))
			return -EINTR;
		wait_for_completion(&work.done);
	}
	return work.result;
}

int mitosis_enable_external(struct task_struct *target)
{
	if (target == current)
		return mitosis_enable(current->mm);

	return mitosis_run_on_target(target, mitosis_enable_task_work_fn);
}

int mitosis_disable_external(struct task_struct *target)
{
	if (target == current) {
		mitosis_disable(current->mm);
		return 0;
	}

	return mitosis_run_on_target(target, mitosis_disable_task_work_fn);
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

