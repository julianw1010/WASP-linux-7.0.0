#ifndef _ASM_X86_MITOSIS_H
#define _ASM_X86_MITOSIS_H

#include <linux/types.h>
#include <linux/nodemask.h>
#include <linux/mm_types.h>
#include <asm/pgtable_types.h>
#include <linux/atomic.h>
#include <linux/spinlock_types.h>
#include <linux/topology.h>
#include <linux/bitmap.h>
#include <linux/jump_label.h>

struct ctl_table;
struct vm_area_struct;
struct seq_file;

DECLARE_STATIC_KEY_FALSE(mitosis_repl_ever_enabled);

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

pmd_t mitosis_pmdp_huge_get_and_clear(struct mm_struct *mm, pmd_t *pmdp);

void mitosis_pmdp_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pmd_t *pmdp);

void mitosis_free_pte_replicas(struct mm_struct *mm, struct page *page);

pmd_t mitosis_pmdp_establish(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd);

int mitosis_pmdp_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pmd_t *pmdp);

pmd_t mitosis_get_pmd(pmd_t *pmdp);

extern struct mitosis_cache_head mitosis_cache[NUMA_NODE_COUNT];

bool mitosis_cache_push(struct page *page, int node, int level,
			struct mm_struct *owner_mm);

struct page *mitosis_cache_pop(int node, int level, struct mm_struct *owner_mm);

int mitosis_cache_drain_node(int node);

int mitosis_cache_drain_all(void);

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
#include <asm/pti.h>
#endif

extern int sysctl_mitosis_inherit;

int mitosis_enable(struct mm_struct *mm);
void mitosis_disable(struct mm_struct *mm);
void mitosis_set_pgd(pgd_t *pgd, pgd_t pgdval);
void mitosis_set_p4d(p4d_t *p4d, p4d_t p4dval);
void mitosis_set_pud(pud_t *pud, pud_t pudval);
void mitosis_set_pmd(pmd_t *pmd, pmd_t pmdval);
void mitosis_set_pte(pte_t *pte, pte_t pteval);
pte_t mitosis_get_pte(pte_t *ptep);

void mitosis_force_steering_switch(struct mm_struct *mm,
					nodemask_t *changed_nodes);

void mitosis_alloc_pte(struct mm_struct *mm, unsigned long pfn);
void mitosis_alloc_pmd(struct mm_struct *mm, unsigned long pfn);
void mitosis_alloc_pud(struct mm_struct *mm, unsigned long pfn);
void mitosis_alloc_p4d(struct mm_struct *mm, unsigned long pfn);

void mitosis_release_pte(unsigned long pfn);
void mitosis_release_pmd(unsigned long pfn);
void mitosis_release_pud(unsigned long pfn);
void mitosis_release_p4d(unsigned long pfn);

int mitosis_inherit_sysctl_handler(struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos);

pte_t mitosis_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep);

int mitosis_enable_external(struct task_struct *target);

int mitosis_disable_external(struct task_struct *target);

void mitosis_ptep_set_wrprotect(struct mm_struct *mm,
				     unsigned long addr, pte_t *ptep);
int mitosis_ptep_test_and_clear_young(struct vm_area_struct *vma,
					   unsigned long addr, pte_t *ptep);

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

struct page *mitosis_get_replica_for_node(struct page *base, int target_node);
bool mitosis_link_page_replicas(struct page **pages, int count);

int mitosis_alloc_pte_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int mitosis_alloc_pmd_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int mitosis_alloc_pud_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int mitosis_alloc_p4d_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int mitosis_alloc_pgd_replicas(struct page *base_page, struct mm_struct *mm,
		       struct page **pages, int *count);
int mitosis_free_replica_chain(struct page *primary, int level, int order);

void mitosis_pt_account_mm(struct mm_struct *mm, int node, int level, int delta);
void mitosis_pt_account_page(struct page *page, int level, int delta);

extern int mitosis_verify;
void mitosis_verify_fault_addr(struct mm_struct *mm, unsigned long address);

int mitosis_audit_run(pid_t pid);
void mitosis_audit_seq_show(struct seq_file *m);
#endif
