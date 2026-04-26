/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PGTABLE_REPL_H
#define _ASM_X86_PGTABLE_REPL_H

#include <linux/types.h>
#include <linux/nodemask.h>
#include <linux/mm_types.h>
#include <asm/pgtable_types.h>
#include <linux/atomic.h>
#include <linux/spinlock_types.h>
#include <linux/topology.h>
#include <linux/bitmap.h>
#include <linux/sysctl.h>

struct vm_area_struct;

#define MITOSIS_CACHE_PTE   0
#define MITOSIS_CACHE_PMD   1
#define MITOSIS_CACHE_PUD   2
#define MITOSIS_CACHE_P4D   3
#define MITOSIS_CACHE_PGD   4

struct mitosis_cache_head {
	spinlock_t lock;
	struct page *head;
	atomic_t count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
} ____cacheline_aligned_in_smp;

pmd_t pgtable_repl_pmdp_huge_get_and_clear(struct mm_struct *mm,
					    pmd_t *pmdp);

void pgtable_repl_pmdp_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pmd_t *pmdp);

void pgtable_repl_free_pte_replicas(struct mm_struct *mm, struct page *page);

void mitosis_defer_pte_page_free(struct mm_struct *mm, struct page *page);
void mitosis_drain_deferred_pages(struct mm_struct *mm);

pmd_t pgtable_repl_pmdp_establish(struct mm_struct *mm, pmd_t *pmdp,
				  pmd_t pmd);

int pgtable_repl_pmdp_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr,
					   pmd_t *pmdp);

pmd_t pgtable_repl_get_pmd(pmd_t *pmdp);

extern struct mitosis_cache_head mitosis_cache[NUMA_NODE_COUNT];

bool mitosis_cache_push(struct page *page, int node, int level);

struct page *mitosis_cache_pop(int node, int level);

int mitosis_cache_drain_node(int node);

int mitosis_cache_drain_all(void);

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
#include <asm/pti.h>
#endif

extern int sysctl_mitosis_mode;
extern int sysctl_mitosis_inherit;

void pgtable_repl_cr3_intercept(unsigned long cr3);
int pgtable_repl_enable(struct mm_struct *mm, nodemask_t nodes);
void pgtable_repl_disable(struct mm_struct *mm);
void pgtable_repl_set_pgd(pgd_t *pgd, pgd_t pgdval);
void pgtable_repl_set_p4d(p4d_t *p4d, p4d_t p4dval);
void pgtable_repl_set_pud(pud_t *pud, pud_t pudval);
void pgtable_repl_set_pmd(pmd_t *pmd, pmd_t pmdval);
void pgtable_repl_set_pte(pte_t *pte, pte_t pteval);
pte_t pgtable_repl_get_pte(pte_t *ptep);
bool mitosis_should_auto_enable(void);

void pgtable_repl_force_steering_switch(struct mm_struct *mm,
					nodemask_t *changed_nodes);

void pgtable_repl_alloc_pte(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_pmd(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_pud(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_alloc_p4d(struct mm_struct *mm, unsigned long pfn);

void pgtable_repl_release_pte(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_release_pmd(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_release_pud(struct mm_struct *mm, unsigned long pfn);
void pgtable_repl_release_p4d(struct mm_struct *mm, unsigned long pfn);

int mitosis_sysctl_handler(const struct ctl_table *table, int write,
			   void *buffer, size_t *lenp, loff_t *ppos);
int mitosis_inherit_sysctl_handler(const struct ctl_table *table, int write,
				   void *buffer, size_t *lenp,
				   loff_t *ppos);

pte_t pgtable_repl_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep);

int pgtable_repl_enable_external(struct task_struct *target, nodemask_t nodes);

int pgtable_repl_disable_external(struct task_struct *target);

void pgtable_repl_ptep_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pte_t *ptep);
int pgtable_repl_ptep_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr,
					   pte_t *ptep);

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
static inline bool mitosis_pti_active(void)
{
	return static_cpu_has(X86_FEATURE_PTI);
}
#else
static inline bool mitosis_pti_active(void)
{
	return false;
}
#endif

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
static inline pgd_t *mitosis_kernel_to_user_pgd(pgd_t *kernel_pgd)
{
	return (pgd_t *)((unsigned long)kernel_pgd + PAGE_SIZE);
}
#else
static inline pgd_t *mitosis_kernel_to_user_pgd(pgd_t *kernel_pgd)
{
	return NULL;
}
#endif

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
static inline pgd_t *mitosis_user_to_kernel_pgd(pgd_t *user_pgd)
{
	return (pgd_t *)((unsigned long)user_pgd - PAGE_SIZE);
}
#else
static inline pgd_t *mitosis_user_to_kernel_pgd(pgd_t *user_pgd)
{
	return NULL;
}
#endif

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
static inline bool mitosis_is_user_pgd(pgd_t *pgd)
{
	return ((unsigned long)pgd & PAGE_SIZE) != 0;
}
#else
static inline bool mitosis_is_user_pgd(pgd_t *pgd)
{
	return false;
}
#endif

static inline int mitosis_pgd_alloc_order(void)
{
	return mitosis_pti_active() ? 1 : 0;
}

static inline pgd_t *mitosis_get_user_pgd_entry(pgd_t *kernel_pgdp)
{
	unsigned long offset;
	pgd_t *kernel_pgd_base;
	int index;
	const int user_kernel_boundary = 256;

	if (!mitosis_pti_active())
		return NULL;

	offset = ((unsigned long)kernel_pgdp) & (PAGE_SIZE - 1);
	index = offset / sizeof(pgd_t);

	if (index >= user_kernel_boundary)
		return NULL;

	kernel_pgd_base = (pgd_t *)((unsigned long)kernel_pgdp & PAGE_MASK);
	return (pgd_t *)((unsigned long)kernel_pgd_base + PAGE_SIZE + offset);
}

int mitosis_interleave_node(struct mm_struct *mm);

struct page *get_replica_for_node(struct page *base, int target_node);
bool link_page_replicas(struct page **pages, int count);
struct page *mitosis_alloc_replica_page(int node, int order);

int alloc_pte_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int alloc_pmd_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int alloc_pud_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int alloc_p4d_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int alloc_pgd_replicas(struct page *base_page, struct mm_struct *mm,
		       nodemask_t nodes, struct page **pages, int *count);
int free_replica_chain_safe(struct page *primary_page,
			    const char *level_name, int order);

extern int sysctl_mitosis_verify_enabled;
void mitosis_verify_fault_walk(struct mm_struct *mm, unsigned long address);

#endif /* _ASM_X86_PGTABLE_REPL_H */
