// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/mempolicy.h>
#include <asm/tlbflush.h>
#include <asm/pgtable_repl.h>

struct steering_switch_info {
	struct mm_struct *mm;
	int initiating_cpu;
};

static void steering_switch_cr3_ipi(void *info)
{
	struct steering_switch_info *switch_info = info;
	struct mm_struct *mm;
	int local_node, target_node;
	pgd_t *target_pgd;
	unsigned long current_cr3_pa, target_cr3_pa, new_cr3;

	if (!switch_info || !switch_info->mm)
		return;

	mm = switch_info->mm;

	if (current->mm != mm && current->active_mm != mm)
		return;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	local_node = numa_node_id();
	target_node = READ_ONCE(mm->repl_steering[local_node]);

	if (target_node < 0 || target_node >= NUMA_NODE_COUNT)
		target_node = local_node;

	target_pgd = READ_ONCE(mm->pgd_replicas[target_node]);
	if (!target_pgd)
		target_pgd = mm->pgd;

	current_cr3_pa = __read_cr3() & PAGE_MASK;
	target_cr3_pa = __pa(target_pgd);

	if (current_cr3_pa != target_cr3_pa) {
		new_cr3 = target_cr3_pa | (__read_cr3() & ~PAGE_MASK);
		native_write_cr3(new_cr3);
		__flush_tlb_all();
	}
}

void pgtable_repl_force_steering_switch(struct mm_struct *mm,
					nodemask_t *changed_nodes)
{
	struct steering_switch_info switch_info;
	unsigned long flags;
	cpumask_var_t target_cpus;
	int cpu;

	/* Pairs with smp_store_release() in pgtable_repl_enable() */
	if (!mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	switch_info.mm = mm;
	switch_info.initiating_cpu = smp_processor_id();

	if (!changed_nodes) {
		local_irq_save(flags);
		if (current->mm == mm || current->active_mm == mm)
			steering_switch_cr3_ipi(&switch_info);
		local_irq_restore(flags);

		on_each_cpu_mask(mm_cpumask(mm),
				 steering_switch_cr3_ipi,
				 &switch_info, 1);
		return;
	}

	if (!alloc_cpumask_var(&target_cpus, GFP_ATOMIC)) {
		local_irq_save(flags);
		if (current->mm == mm || current->active_mm == mm)
			steering_switch_cr3_ipi(&switch_info);
		local_irq_restore(flags);

		on_each_cpu_mask(mm_cpumask(mm),
				 steering_switch_cr3_ipi,
				 &switch_info, 1);
		return;
	}

	cpumask_clear(target_cpus);
	for_each_cpu(cpu, mm_cpumask(mm)) {
		int node = cpu_to_node(cpu);

		if (node >= 0 && node < NUMA_NODE_COUNT &&
		    node_isset(node, *changed_nodes))
			cpumask_set_cpu(cpu, target_cpus);
	}

	local_irq_save(flags);
	if (current->mm == mm || current->active_mm == mm) {
		int my_node = numa_node_id();

		if (my_node >= 0 && my_node < NUMA_NODE_COUNT &&
		    node_isset(my_node, *changed_nodes))
			steering_switch_cr3_ipi(&switch_info);
	}
	local_irq_restore(flags);

	on_each_cpu_mask(target_cpus, steering_switch_cr3_ipi,
			 &switch_info, 1);
	free_cpumask_var(target_cpus);
}
EXPORT_SYMBOL(pgtable_repl_force_steering_switch);

int mitosis_interleave_node(struct mm_struct *mm)
{
	struct mempolicy *pol;
	int idx, node, i, num_nodes;

	if (!mm)
		return -1;

	pol = current->mempolicy;
	if (!pol || pol->mode != MPOL_INTERLEAVE)
		return -1;

	num_nodes = nodes_weight(pol->nodes);
	if (num_nodes <= 1)
		return -1;

	idx = atomic_inc_return(&mm->pgtable_interleave_counter) - 1;
	idx = idx % num_nodes;

	i = 0;
	for_each_node_mask(node, pol->nodes) {
		if (i == idx)
			return node;
		i++;
	}

	return -1;
}
EXPORT_SYMBOL(mitosis_interleave_node);
