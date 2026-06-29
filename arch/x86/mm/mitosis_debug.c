#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <asm/pgtable.h>
#include <asm/mitosis.h>

int mitosis_verify __read_mostly;

static void mitosis_verify_check_child(struct mm_struct *mm, int node,
				       const char *level, unsigned long entry_val)
{
	unsigned long phys = entry_val & PTE_PFN_MASK;
	struct page *child;
	int child_node;

	if (!phys)
		return;

	child = pfn_to_page(phys >> PAGE_SHIFT);
	child_node = page_to_nid(child);
	if (child_node != node) {
		pr_emerg("MITOSIS verify: mm %px node %d %s table on node %d (cross-node)\n",
			 mm, node, level, child_node);
		BUG();
	}
}

static void mitosis_verify_tree(struct mm_struct *mm, int node, pgd_t *node_pgd)
{
	int pgd_idx, p4d_idx, pud_idx, pmd_idx;

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pgdval = READ_ONCE(node_pgd[pgd_idx]);
		p4d_t *p4d_base;

		if (pgd_none(pgdval) || !pgd_present(pgdval))
			continue;

		if (pgtable_l5_enabled())
			mitosis_verify_check_child(mm, node, "P4D", pgd_val(pgdval));

		p4d_base = p4d_offset(&node_pgd[pgd_idx], 0);

		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4dval = READ_ONCE(p4d_base[p4d_idx]);
			pud_t *pud_base;

			if (p4d_none(p4dval) || !p4d_present(p4dval))
				continue;

			mitosis_verify_check_child(mm, node, "PUD", p4d_val(p4dval));

			pud_base = pud_offset(&p4d_base[p4d_idx], 0);

			for (pud_idx = 0; pud_idx < PTRS_PER_PUD; pud_idx++) {
				pud_t pudval = READ_ONCE(pud_base[pud_idx]);
				pmd_t *pmd_base;

				if (pud_none(pudval) || !pud_present(pudval) ||
				    pud_trans_huge(pudval))
					continue;

				mitosis_verify_check_child(mm, node, "PMD",
							   pud_val(pudval));

				pmd_base = pmd_offset(&pud_base[pud_idx], 0);

				for (pmd_idx = 0; pmd_idx < PTRS_PER_PMD; pmd_idx++) {
					pmd_t pmdval = READ_ONCE(pmd_base[pmd_idx]);

					if (pmd_none(pmdval) || !pmd_present(pmdval) ||
					    pmd_trans_huge(pmdval))
						continue;

					mitosis_verify_check_child(mm, node, "PTE",
								   pmd_val(pmdval));
				}
			}
		}
	}
}

void mitosis_verify_locality(struct mm_struct *mm)
{
	int node;

	if (!mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *node_pgd;
		int pgd_node;

		if (!node_isset(node, mm->repl_pgd_nodes))
			continue;

		node_pgd = READ_ONCE(mm->pgd_replicas[node]);
		if (!node_pgd) {
			pr_emerg("MITOSIS verify: mm %px node %d has no pgd replica\n",
				 mm, node);
			BUG();
		}

		pgd_node = page_to_nid(virt_to_page(node_pgd));
		if (pgd_node != node) {
			pr_emerg("MITOSIS verify: mm %px node %d pgd on node %d\n",
				 mm, node, pgd_node);
			BUG();
		}

		mitosis_verify_tree(mm, node, node_pgd);
	}
}
