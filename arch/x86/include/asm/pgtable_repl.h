#ifndef _ASM_X86_PGTABLE_REPL_H
#define _ASM_X86_PGTABLE_REPL_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/nodemask.h>
#include <linux/mm_types.h>
#include <asm/pgtable_types.h>

extern int sysctl_wasp_auto_enable;

struct vm_area_struct;

int pgtable_repl_enable(struct mm_struct *mm, nodemask_t nodes);
void pgtable_repl_disable(struct mm_struct *mm);

void pgtable_repl_set_pgd(pgd_t *pgd, pgd_t pgdval);
void pgtable_repl_set_p4d(p4d_t *p4d, p4d_t p4dval);
void pgtable_repl_set_pud(pud_t *pud, pud_t pudval);
void pgtable_repl_set_pmd(pmd_t *pmd, pmd_t pmdval);
void pgtable_repl_set_pte(pte_t *pte, pte_t pteval);

pte_t pgtable_repl_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep);
void pgtable_repl_ptep_set_wrprotect(struct mm_struct *mm,
                                     unsigned long addr, pte_t *ptep);
int pgtable_repl_ptep_test_and_clear_young(struct vm_area_struct *vma,
                                           unsigned long addr, pte_t *ptep);
pte_t pgtable_repl_get_pte(pte_t *ptep);

void pgtable_repl_free_pte_replicas(struct mm_struct *mm, struct page *page);

void pgtable_repl_alloc_pte(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_pmd(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_pud(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn);

void pgtable_repl_release_pte(unsigned long pfn);
void pgtable_repl_release_pmd(unsigned long pfn);
void pgtable_repl_release_pud(unsigned long pfn);
void pgtable_repl_release_p4d(unsigned long pfn);

void pgtable_repl_force_steering_switch(struct mm_struct *mm, nodemask_t *changed_nodes);

#endif
#endif
