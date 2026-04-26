// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/pgtable.h>
#include <asm/pgtable_repl.h>

int sysctl_mitosis_verify_enabled;
EXPORT_SYMBOL(sysctl_mitosis_verify_enabled);

void mitosis_verify_fault_walk(struct mm_struct *mm, unsigned long address)
{
	unsigned long cr3_pa;
	pgd_t *pgd_base;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int expected_node;
	int pg_node;

	if (!sysctl_mitosis_verify_enabled)
		return;

	cr3_pa = __read_cr3() & PAGE_MASK;
	if (!cr3_pa || !pfn_valid(cr3_pa >> PAGE_SHIFT))
		return;

	pgd_base = __va(cr3_pa);
	expected_node = page_to_nid(pfn_to_page(cr3_pa >> PAGE_SHIFT));

	if (expected_node < 0 || expected_node >= NUMA_NODE_COUNT)
		return;

	pgd = pgd_offset_pgd(pgd_base, address);
	if (pgd_none(*pgd) || !pgd_present(*pgd))
		return;

	if (pgtable_l5_enabled()) {
		unsigned long child_phys;

		child_phys = pgd_val(*pgd) & PTE_PFN_MASK;
		if (child_phys && pfn_valid(child_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(
				pfn_to_page(child_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("MITOSIS: fault verify: P4D on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				WARN_ON_ONCE(1);
				return;
			}
		}
	}

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || !p4d_present(*p4d))
		return;

	{
		unsigned long pud_phys;

		pud_phys = p4d_val(*p4d) & PTE_PFN_MASK;
		if (pud_phys && pfn_valid(pud_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(
				pfn_to_page(pud_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("MITOSIS: fault verify: PUD on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				WARN_ON_ONCE(1);
				return;
			}
		}
	}

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || !pud_present(*pud) || pud_trans_huge(*pud))
		return;

	{
		unsigned long pmd_phys;

		pmd_phys = pud_val(*pud) & PTE_PFN_MASK;
		if (pmd_phys && pfn_valid(pmd_phys >> PAGE_SHIFT)) {
			pg_node = page_to_nid(
				pfn_to_page(pmd_phys >> PAGE_SHIFT));
			if (pg_node != expected_node) {
				pr_err("MITOSIS: fault verify: PMD on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
				       pg_node, expected_node, address,
				       current->comm, current->pid);
				WARN_ON_ONCE(1);
				return;
			}
		}
	}

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || !pmd_present(*pmd) || pmd_trans_huge(*pmd))
		return;

	pte = pte_offset_kernel(pmd, address);
	if (!virt_addr_valid(pte))
		return;

	pg_node = page_to_nid(virt_to_page(pte));
	if (pg_node != expected_node) {
		pr_err("MITOSIS: fault verify: PTE on node %d expected %d addr=0x%lx comm=%s pid=%d\n",
		       pg_node, expected_node, address,
		       current->comm, current->pid);
		WARN_ON_ONCE(1);
	}
}
EXPORT_SYMBOL(mitosis_verify_fault_walk);
