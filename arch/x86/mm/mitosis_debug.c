#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/printk.h>
#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/mitosis.h>

int mitosis_verify __read_mostly;

static void mitosis_verify_addr_check(struct mm_struct *mm, int node,
				      const char *level, void *entryp,
				      unsigned long address)
{
	int pnode = page_to_nid(virt_to_page(entryp));

	if (pnode != node) {
		pr_emerg("MITOSIS verify: mm %px addr %lx %s table on node %d (cr3 pgd node %d)\n",
			 mm, address, level, pnode, node);
		BUG();
	}
}

void mitosis_verify_fault_addr(struct mm_struct *mm, unsigned long address)
{
	int node;
	pgd_t *pgd_base, *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pgd_t pgdval;
	p4d_t p4dval;
	pud_t pudval;
	pmd_t pmdval;

	if (!mm || !smp_load_acquire(&mm->repl_pgd_enabled))
		return;

	pgd_base = __va(read_cr3_pa());
	node = page_to_nid(virt_to_page(pgd_base));

	pgd = pgd_offset_pgd(pgd_base, address);
	pgdval = READ_ONCE(*pgd);
	if (pgd_none(pgdval) || !pgd_present(pgdval))
		return;

	p4d = p4d_offset(pgd, address);
	if (pgtable_l5_enabled())
		mitosis_verify_addr_check(mm, node, "P4D", p4d, address);
	p4dval = READ_ONCE(*p4d);
	if (p4d_none(p4dval) || !p4d_present(p4dval))
		return;

	pud = pud_offset(p4d, address);
	mitosis_verify_addr_check(mm, node, "PUD", pud, address);
	pudval = READ_ONCE(*pud);
	if (pud_none(pudval) || !pud_present(pudval) || pud_trans_huge(pudval))
		return;

	pmd = pmd_offset(pud, address);
	mitosis_verify_addr_check(mm, node, "PMD", pmd, address);
	pmdval = READ_ONCE(*pmd);
	if (pmd_none(pmdval) || !pmd_present(pmdval) || pmd_trans_huge(pmdval))
		return;

	mitosis_verify_addr_check(mm, node, "PTE",
				  pte_offset_kernel(pmd, address), address);
}
