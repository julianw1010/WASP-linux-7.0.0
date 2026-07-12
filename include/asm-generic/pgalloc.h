/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_PGALLOC_H
#define __ASM_GENERIC_PGALLOC_H

#ifdef CONFIG_MMU

#define GFP_PGTABLE_KERNEL	(GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER	(GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

#include <asm/mitosis.h>

/**
 * __pte_alloc_one_kernel - allocate memory for a PTE-level kernel page table
 * @mm: the mm_struct of the current context
 *
 * This function is intended for architectures that need
 * anything beyond simple page allocation.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pte_t *__pte_alloc_one_kernel_noprof(struct mm_struct *mm)
{
	struct ptdesc *ptdesc = pagetable_alloc_noprof(GFP_PGTABLE_KERNEL, 0);

	if (!ptdesc)
		return NULL;
	if (!pagetable_pte_ctor(mm, ptdesc)) {
		pagetable_free(ptdesc);
		return NULL;
	}

	ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define __pte_alloc_one_kernel(...)	alloc_hooks(__pte_alloc_one_kernel_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE_KERNEL
/**
 * pte_alloc_one_kernel - allocate memory for a PTE-level kernel page table
 * @mm: the mm_struct of the current context
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pte_t *pte_alloc_one_kernel_noprof(struct mm_struct *mm)
{
	return __pte_alloc_one_kernel_noprof(mm);
}
#define pte_alloc_one_kernel(...)	alloc_hooks(pte_alloc_one_kernel_noprof(__VA_ARGS__))
#endif

/**
 * pte_free_kernel - free PTE-level kernel page table memory
 * @mm: the mm_struct of the current context
 * @pte: pointer to the memory containing the page table
 */
static inline void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
	pagetable_dtor_free(virt_to_ptdesc(pte));
}

/**
 * __pte_alloc_one - allocate memory for a PTE-level user page table
 * @mm: the mm_struct of the current context
 * @gfp: GFP flags to use for the allocation
 *
 * Allocate memory for a page table and ptdesc and runs pagetable_pte_ctor().
 *
 * This function is intended for architectures that need
 * anything beyond simple page allocation or must have custom GFP flags.
 *
 * Return: `struct page` referencing the ptdesc or %NULL on error
 */
static inline pgtable_t __pte_alloc_one_noprof(struct mm_struct *mm, gfp_t gfp,
					       pmd_t *pmd)
{
	int node = page_to_nid(virt_to_page(pmd));
	struct ptdesc *ptdesc;
	struct page *page;

	page = mitosis_cache_pop(node, MITOSIS_CACHE_PTE, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	BUG_ON(!page);

	ptdesc = page_ptdesc(page);
	BUG_ON(!pagetable_pte_ctor(mm, ptdesc));

	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_PTE, 1);
	return ptdesc_page(ptdesc);
}
#define __pte_alloc_one(...)	alloc_hooks(__pte_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE
/**
 * pte_alloc_one - allocate a page for PTE-level user page table
 * @mm: the mm_struct of the current context
 *
 * Allocate memory for a page table and ptdesc and runs pagetable_pte_ctor().
 *
 * Return: `struct page` referencing the ptdesc or %NULL on error
 */
static inline pgtable_t pte_alloc_one_noprof(struct mm_struct *mm, pmd_t *pmd)
{
	return __pte_alloc_one_noprof(mm, GFP_PGTABLE_USER, pmd);
}
#define pte_alloc_one(...)	alloc_hooks(pte_alloc_one_noprof(__VA_ARGS__))
#endif

static inline void mitosis_dtor_free_page(struct page *page, int level)
{
	struct ptdesc *ptdesc = page_ptdesc(page);
	int nid = page_to_nid(page);
	bool from_cache = PageMitosisFromCache(page);
	struct mm_struct *owner_mm = page->pt_owner_mm;

	mitosis_pt_account_page(page, level, -1);

	pagetable_dtor(ptdesc);

	page->pt_owner_mm = NULL;
	ClearPageMitosisFromCache(page);

	if (from_cache) {
		page->pt_replica = NULL;
		if (mitosis_cache_push(page, nid, level, owner_mm))
			return;
	}

	pagetable_free(ptdesc);
}

/*
 * Should really implement gc for free page table pages. This could be
 * done with a reference count in struct page.
 */

/**
 * pte_free - free PTE-level user page table memory
 * @mm: the mm_struct of the current context
 * @pte_page: the `struct page` referencing the ptdesc
 */
static inline void pte_free(struct mm_struct *mm, struct page *pte_page)
{
	mitosis_free_pte_replicas(mm, pte_page);
	mitosis_dtor_free_page(pte_page, MITOSIS_CACHE_PTE);
}

#if CONFIG_PGTABLE_LEVELS > 2

#ifndef __HAVE_ARCH_PMD_ALLOC_ONE
/**
 * pmd_alloc_one - allocate memory for a PMD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocate memory for a page table and ptdesc and runs pagetable_pmd_ctor().
 *
 * Allocations use %GFP_PGTABLE_USER in user context and
 * %GFP_PGTABLE_KERNEL in kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pmd_t *pmd_alloc_one_noprof(struct mm_struct *mm, unsigned long addr,
					  pud_t *pud)
{
	gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	struct ptdesc *ptdesc;
	struct page *page;
	int node;

	if (mm == &init_mm) {
		ptdesc = pagetable_alloc_noprof(gfp, 0);
		if (!ptdesc)
			return NULL;
		if (!pagetable_pmd_ctor(mm, ptdesc)) {
			pagetable_free(ptdesc);
			return NULL;
		}
		ptdesc_set_kernel(ptdesc);
		return ptdesc_address(ptdesc);
	}

	node = page_to_nid(virt_to_page(pud));
	page = mitosis_cache_pop(node, MITOSIS_CACHE_PMD, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	BUG_ON(!page);

	ptdesc = page_ptdesc(page);
	BUG_ON(!pagetable_pmd_ctor(mm, ptdesc));

	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_PMD, 1);
	return ptdesc_address(ptdesc);
}
#define pmd_alloc_one(...)	alloc_hooks(pmd_alloc_one_noprof(__VA_ARGS__))
#endif

#ifndef __HAVE_ARCH_PMD_FREE
static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	BUG_ON((unsigned long)pmd & (PAGE_SIZE-1));
	mitosis_dtor_free_page(ptdesc_page(virt_to_ptdesc(pmd)), MITOSIS_CACHE_PMD);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 2 */

#if CONFIG_PGTABLE_LEVELS > 3

static inline pud_t *__pud_alloc_one_noprof(struct mm_struct *mm, unsigned long addr,
					    p4d_t *p4d)
{
	gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	struct ptdesc *ptdesc;
	struct page *page;
	int node;

	if (mm == &init_mm) {
		ptdesc = pagetable_alloc_noprof(gfp, 0);
		if (!ptdesc)
			return NULL;
		pagetable_pud_ctor(ptdesc);
		ptdesc_set_kernel(ptdesc);
		return ptdesc_address(ptdesc);
	}

	node = page_to_nid(virt_to_page(p4d));
	page = mitosis_cache_pop(node, MITOSIS_CACHE_PUD, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_ZERO | __GFP_THISNODE, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	pagetable_pud_ctor(ptdesc);
	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_PUD, 1);
	return ptdesc_address(ptdesc);
}
#define __pud_alloc_one(...)	alloc_hooks(__pud_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PUD_ALLOC_ONE
/**
 * pud_alloc_one - allocate memory for a PUD-level page table
 * @mm: the mm_struct of the current context
 *
 * Allocate memory for a page table using %GFP_PGTABLE_USER for user context
 * and %GFP_PGTABLE_KERNEL for kernel context.
 *
 * Return: pointer to the allocated memory or %NULL on error
 */
static inline pud_t *pud_alloc_one_noprof(struct mm_struct *mm, unsigned long addr,
					  p4d_t *p4d)
{
	return __pud_alloc_one_noprof(mm, addr, p4d);
}
#define pud_alloc_one(...)	alloc_hooks(pud_alloc_one_noprof(__VA_ARGS__))
#endif

static inline void __pud_free(struct mm_struct *mm, pud_t *pud)
{
	BUG_ON((unsigned long)pud & (PAGE_SIZE-1));
	mitosis_dtor_free_page(ptdesc_page(virt_to_ptdesc(pud)), MITOSIS_CACHE_PUD);
}

#ifndef __HAVE_ARCH_PUD_FREE
static inline void pud_free(struct mm_struct *mm, pud_t *pud)
{
	__pud_free(mm, pud);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 3 */

#if CONFIG_PGTABLE_LEVELS > 4

static inline p4d_t *__p4d_alloc_one_noprof(struct mm_struct *mm, unsigned long addr,
					    pgd_t *pgd)
{
	gfp_t gfp = (mm == &init_mm) ? GFP_PGTABLE_KERNEL : GFP_PGTABLE_USER;
	struct ptdesc *ptdesc;
	struct page *page;
	int node;

	if (mm == &init_mm) {
		ptdesc = pagetable_alloc_noprof(gfp, 0);
		if (!ptdesc)
			return NULL;
		pagetable_p4d_ctor(ptdesc);
		ptdesc_set_kernel(ptdesc);
		return ptdesc_address(ptdesc);
	}

	node = page_to_nid(virt_to_page(pgd));
	page = mitosis_cache_pop(node, MITOSIS_CACHE_P4D, mm);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_ZERO | __GFP_THISNODE, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	pagetable_p4d_ctor(ptdesc);
	page->pt_owner_mm = mm;
	mitosis_pt_account_page(page, MITOSIS_CACHE_P4D, 1);
	return ptdesc_address(ptdesc);
}
#define __p4d_alloc_one(...)	alloc_hooks(__p4d_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_P4D_ALLOC_ONE
static inline p4d_t *p4d_alloc_one_noprof(struct mm_struct *mm, unsigned long addr,
					  pgd_t *pgd)
{
	return __p4d_alloc_one_noprof(mm, addr, pgd);
}
#define p4d_alloc_one(...)	alloc_hooks(p4d_alloc_one_noprof(__VA_ARGS__))
#endif

static inline void __p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	BUG_ON((unsigned long)p4d & (PAGE_SIZE-1));
	mitosis_dtor_free_page(ptdesc_page(virt_to_ptdesc(p4d)), MITOSIS_CACHE_P4D);
}

#ifndef __HAVE_ARCH_P4D_FREE
static inline void p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	if (!mm_p4d_folded(mm))
		__p4d_free(mm, p4d);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 4 */

static inline pgd_t *__pgd_alloc_noprof(struct mm_struct *mm, unsigned int order)
{
	gfp_t gfp = GFP_PGTABLE_USER;
	struct ptdesc *ptdesc;

	if (mm == &init_mm)
		gfp = GFP_PGTABLE_KERNEL;

	ptdesc = pagetable_alloc_noprof(gfp, order);
	if (!ptdesc)
		return NULL;

	pagetable_pgd_ctor(ptdesc);

	if (mm == &init_mm)
		ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define __pgd_alloc(...)	alloc_hooks(__pgd_alloc_noprof(__VA_ARGS__))

static inline void __pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pgd);

	BUG_ON((unsigned long)pgd & (PAGE_SIZE-1));
	pagetable_dtor_free(ptdesc);
}

#ifndef __HAVE_ARCH_PGD_FREE
static inline void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	__pgd_free(mm, pgd);
}
#endif

#endif /* CONFIG_MMU */

#endif /* __ASM_GENERIC_PGALLOC_H */
