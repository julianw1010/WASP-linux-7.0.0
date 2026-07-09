#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mitosis.h>
#include <linux/mitosis_stats.h>

#define MITOSIS_AUDIT_AD (_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY)

struct mitosis_audit_result {
	int valid;
	int pid;
	int enabled;
	int nodes;
	long pages[MITOSIS_PT_NR_LEVELS];
	long members[MITOSIS_PT_NR_LEVELS];
	long pt_cur[MITOSIS_PT_NR_LEVELS];
};

static DEFINE_SPINLOCK(mitosis_audit_lock);
static struct mitosis_audit_result mitosis_audit_last;

struct audit_ctx {
	struct mm_struct *mm;
	nodemask_t nodes;
	int nweight;
	long pages[MITOSIS_PT_NR_LEVELS];
	long members[MITOSIS_PT_NR_LEVELS];
};

static void audit_fail(struct mm_struct *mm, unsigned long addr, const char *what,
		       int node, unsigned long expected, unsigned long got)
{
	pr_emerg("MITOSIS audit: mm %px addr %lx %s node %d expected %lx got %lx\n",
		 mm, addr, what, node, expected, got);
	BUG();
}

static void audit_fill_replicas(struct audit_ctx *c, struct page *base,
				unsigned long addr, struct page **arr)
{
	int n;

	for (n = 0; n < NUMA_NODE_COUNT; n++) {
		struct page *r;

		arr[n] = NULL;
		if (!node_isset(n, c->nodes))
			continue;
		r = mitosis_get_replica_for_node(base, n);
		if (!r)
			audit_fail(c->mm, addr, "missing replica on node", n, 0, 0);
		if (page_to_nid(r) != n)
			audit_fail(c->mm, addr, "replica on wrong node", n, n,
				   page_to_nid(r));
		if (r->pt_owner_mm != c->mm)
			audit_fail(c->mm, addr, "replica wrong owner_mm", n,
				   (unsigned long)c->mm,
				   (unsigned long)r->pt_owner_mm);
		arr[n] = r;
	}
}

static void audit_ring(struct audit_ctx *c, struct page *p, int level,
		       unsigned long addr)
{
	struct page *cur, *start;
	nodemask_t seen;
	int count = 0;

	if (!p->pt_replica)
		audit_fail(c->mm, addr, "null replica ring", level, c->nweight, 0);

	nodes_clear(seen);
	start = p;
	cur = p;
	do {
		int n = page_to_nid(cur);

		if (n < 0 || n >= NUMA_NODE_COUNT || !node_isset(n, c->nodes))
			audit_fail(c->mm, addr, "ring member off-set node", n, 0, 0);
		if (node_isset(n, seen))
			audit_fail(c->mm, addr, "ring duplicate node", n, 0, 0);
		if (cur->pt_owner_mm != c->mm)
			audit_fail(c->mm, addr, "ring member wrong owner_mm", n,
				   (unsigned long)c->mm,
				   (unsigned long)cur->pt_owner_mm);
		node_set(n, seen);
		if (++count > c->nweight)
			audit_fail(c->mm, addr, "ring overlong", level,
				   c->nweight, count);
		cur = cur->pt_replica;
	} while (cur && cur != start);

	if (cur != start)
		audit_fail(c->mm, addr, "ring not closed", level, 0, 0);
	if (count != c->nweight)
		audit_fail(c->mm, addr, "ring wrong member count", level,
			   c->nweight, count);
	if (!nodes_equal(seen, c->nodes))
		audit_fail(c->mm, addr, "ring node-set mismatch", level, 0, 0);

	c->pages[level]++;
	c->members[level] += count;
}

static void audit_nonleaf(struct audit_ctx *c, struct page **prep,
			  struct page *child, unsigned long off,
			  unsigned long master_val, unsigned long addr)
{
	unsigned long master_flags = master_val & ~PTE_PFN_MASK & ~MITOSIS_AUDIT_AD;
	int n;

	for (n = 0; n < NUMA_NODE_COUNT; n++) {
		struct page *cn;
		unsigned long ev, eflags, epfn, want;

		if (!node_isset(n, c->nodes))
			continue;
		cn = mitosis_get_replica_for_node(child, n);
		if (!cn || page_to_nid(cn) != n)
			audit_fail(c->mm, addr, "child has no replica on node", n,
				   0, 0);
		ev = READ_ONCE(*(unsigned long *)(page_address(prep[n]) + off));
		eflags = ev & ~PTE_PFN_MASK & ~MITOSIS_AUDIT_AD;
		epfn = ev & PTE_PFN_MASK;
		want = __pa(page_address(cn));
		if (epfn != want)
			audit_fail(c->mm, addr, "nonleaf wrong child pfn", n,
				   want, epfn);
		if (eflags != master_flags)
			audit_fail(c->mm, addr, "nonleaf flag mismatch", n,
				   master_flags, eflags);
	}
}

static void audit_leaf(struct audit_ctx *c, struct page **prep,
		       unsigned long off, unsigned long master_val,
		       unsigned long addr)
{
	unsigned long ref = master_val & ~MITOSIS_AUDIT_AD;
	int n;

	for (n = 0; n < NUMA_NODE_COUNT; n++) {
		unsigned long ev;

		if (!node_isset(n, c->nodes))
			continue;
		ev = READ_ONCE(*(unsigned long *)(page_address(prep[n]) + off)) &
		     ~MITOSIS_AUDIT_AD;
		if (ev != ref)
			audit_fail(c->mm, addr, "leaf replica diverged", n, ref, ev);
	}
}

static void audit_pte_table(struct audit_ctx *c, pmd_t *pmd, struct page *ptp,
			    unsigned long base)
{
	struct page *prep[NUMA_NODE_COUNT];
	spinlock_t *ptl;
	pte_t *pte;
	int i;

	audit_ring(c, ptp, MITOSIS_CACHE_PTE, base);
	audit_fill_replicas(c, ptp, base, prep);

	pte = pte_offset_map_lock(c->mm, pmd, base, &ptl);
	if (!pte)
		return;
	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte_t e = READ_ONCE(pte[i]);

		if (pte_none(e) || !pte_present(e))
			continue;
		audit_leaf(c, prep, (unsigned long)i * sizeof(pte_t),
			   pte_val(e), base + ((unsigned long)i << PAGE_SHIFT));
	}
	pte_unmap_unlock(pte, ptl);
}

static void audit_pmd_table(struct audit_ctx *c, pud_t *pud, struct page *pmp,
			    unsigned long base)
{
	struct page *prep[NUMA_NODE_COUNT];
	pmd_t *pmd_base = pmd_offset(pud, base);
	int i;

	audit_ring(c, pmp, MITOSIS_CACHE_PMD, base);
	audit_fill_replicas(c, pmp, base, prep);

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd_t v = READ_ONCE(pmd_base[i]);
		unsigned long addr = base + ((unsigned long)i << PMD_SHIFT);
		unsigned long off = (unsigned long)i * sizeof(pmd_t);
		struct page *child;

		if (pmd_none(v) || !pmd_present(v))
			continue;
		if (pmd_leaf(v) || pmd_trans_huge(v)) {
			spinlock_t *ptl = pmd_lock(c->mm, &pmd_base[i]);

			audit_leaf(c, prep, off, pmd_val(v), addr);
			spin_unlock(ptl);
			continue;
		}
		child = pfn_to_page((pmd_val(v) & PTE_PFN_MASK) >> PAGE_SHIFT);
		audit_nonleaf(c, prep, child, off, pmd_val(v), addr);
		audit_pte_table(c, &pmd_base[i], child, addr);
	}
}

static void audit_pud_table(struct audit_ctx *c, p4d_t *p4d, struct page *pup,
			    unsigned long base)
{
	struct page *prep[NUMA_NODE_COUNT];
	pud_t *pud_base = pud_offset(p4d, base);
	int i;

	audit_ring(c, pup, MITOSIS_CACHE_PUD, base);
	audit_fill_replicas(c, pup, base, prep);

	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud_t v = READ_ONCE(pud_base[i]);
		unsigned long addr = base + ((unsigned long)i << PUD_SHIFT);
		unsigned long off = (unsigned long)i * sizeof(pud_t);
		struct page *child;

		if (pud_none(v) || !pud_present(v))
			continue;
		if (pud_leaf(v) || pud_trans_huge(v)) {
			spinlock_t *ptl = pud_lock(c->mm, &pud_base[i]);

			audit_leaf(c, prep, off, pud_val(v), addr);
			spin_unlock(ptl);
			continue;
		}
		child = pfn_to_page((pud_val(v) & PTE_PFN_MASK) >> PAGE_SHIFT);
		audit_nonleaf(c, prep, child, off, pud_val(v), addr);
		audit_pmd_table(c, &pud_base[i], child, addr);
	}
}

static void audit_walk(struct audit_ctx *c)
{
	struct mm_struct *mm = c->mm;
	pgd_t *pgd = mm->pgd;
	struct page *pgd_page = virt_to_page(pgd);
	struct page *prep_pgd[NUMA_NODE_COUNT];
	int pgd_idx;

	audit_ring(c, pgd_page, MITOSIS_CACHE_PGD, 0);
	audit_fill_replicas(c, pgd_page, 0, prep_pgd);

	for (pgd_idx = 0; pgd_idx < KERNEL_PGD_BOUNDARY; pgd_idx++) {
		pgd_t pv = READ_ONCE(pgd[pgd_idx]);
		unsigned long a_pgd = (unsigned long)pgd_idx << PGDIR_SHIFT;
		struct page *prep_p4d_storage[NUMA_NODE_COUNT];
		struct page **prep_p4d;
		p4d_t *p4d_base;
		int p4d_idx;

		if (pgd_none(pv) || !pgd_present(pv))
			continue;

		if (pgtable_l5_enabled()) {
			struct page *child =
				pfn_to_page((pgd_val(pv) & PTE_PFN_MASK) >> PAGE_SHIFT);

			audit_nonleaf(c, prep_pgd, child,
				      (unsigned long)pgd_idx * sizeof(pgd_t),
				      pgd_val(pv), a_pgd);
			audit_ring(c, child, MITOSIS_CACHE_P4D, a_pgd);
			audit_fill_replicas(c, child, a_pgd, prep_p4d_storage);
			prep_p4d = prep_p4d_storage;
		} else {
			prep_p4d = prep_pgd;
		}

		p4d_base = p4d_offset(&pgd[pgd_idx], a_pgd);
		for (p4d_idx = 0; p4d_idx < PTRS_PER_P4D; p4d_idx++) {
			p4d_t p4v = READ_ONCE(p4d_base[p4d_idx]);
			unsigned long a_p4d = a_pgd +
				((unsigned long)p4d_idx << P4D_SHIFT);
			unsigned long off =
				((unsigned long)&p4d_base[p4d_idx]) & ~PAGE_MASK;
			struct page *child_pud;

			if (p4d_none(p4v) || !p4d_present(p4v))
				continue;
			child_pud = pfn_to_page((p4d_val(p4v) & PTE_PFN_MASK) >>
						PAGE_SHIFT);
			audit_nonleaf(c, prep_p4d, child_pud, off, p4d_val(p4v),
				      a_p4d);
			audit_pud_table(c, &p4d_base[p4d_idx], child_pud, a_p4d);
		}
	}
}

int mitosis_audit_run(pid_t pid)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm;
	struct audit_ctx c;
	struct mitosis_audit_result res;
	int target_pid;
	int level, node;

	if (pid == 0) {
		mm = current->mm;
		if (mm)
			mmget(mm);
		target_pid = current->tgid;
	} else {
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (!task)
			return -ESRCH;
		mm = get_task_mm(task);
		target_pid = pid;
	}

	if (!mm) {
		if (task)
			put_task_struct(task);
		return -EINVAL;
	}

	memset(&res, 0, sizeof(res));
	res.valid = 1;
	res.pid = target_pid;

	mutex_lock(&mm->repl_mutex);

	if (smp_load_acquire(&mm->repl_pgd_enabled)) {
		struct mitosis_stats *s;

		memset(&c, 0, sizeof(c));
		c.mm = mm;
		c.nodes = mm->repl_pgd_nodes;
		c.nweight = nodes_weight(c.nodes);

		mmap_write_lock(mm);
		audit_walk(&c);
		mmap_write_unlock(mm);

		res.enabled = 1;
		res.nodes = c.nweight;
		for (level = 0; level < MITOSIS_PT_NR_LEVELS; level++) {
			res.pages[level] = c.pages[level];
			res.members[level] = c.members[level];
		}

		s = mm->mitosis_stats;
		if (s) {
			for (level = 0; level < MITOSIS_PT_NR_LEVELS; level++)
				for (node = 0; node < NUMA_NODE_COUNT; node++)
					res.pt_cur[level] += atomic_long_read(
						&s->pt_cur[node][level]);
		}
	}

	mutex_unlock(&mm->repl_mutex);

	spin_lock(&mitosis_audit_lock);
	mitosis_audit_last = res;
	spin_unlock(&mitosis_audit_lock);

	mmput(mm);
	if (task)
		put_task_struct(task);
	return 0;
}

void mitosis_audit_seq_show(struct seq_file *m)
{
	static const char * const lvl[MITOSIS_PT_NR_LEVELS] = {
		[MITOSIS_CACHE_PTE] = "PTE",
		[MITOSIS_CACHE_PMD] = "PMD",
		[MITOSIS_CACHE_PUD] = "PUD",
		[MITOSIS_CACHE_P4D] = "P4D",
		[MITOSIS_CACHE_PGD] = "PGD",
	};
	struct mitosis_audit_result r;
	int level;

	spin_lock(&mitosis_audit_lock);
	r = mitosis_audit_last;
	spin_unlock(&mitosis_audit_lock);

	if (!r.valid) {
		seq_puts(m, " no audit run yet (write a pid to trigger; 0 = writer's own mm)\n");
		return;
	}

	seq_puts(m, " Mitosis page-table audit  (last run)\n");
	seq_puts(m, " --------------------------------------------------------------\n");
	seq_printf(m, "   %-22s %d\n", "target pid", r.pid);
	seq_printf(m, "   %-22s %s\n", "replication enabled", r.enabled ? "yes" : "no");
	if (!r.enabled) {
		seq_puts(m, "   (nothing to audit)\n");
		return;
	}
	seq_printf(m, "   %-22s %d\n", "replica nodes", r.nodes);
	seq_printf(m, "   %-22s %s\n", "result",
		   "CLEAN (every invariant held; a violation BUGs the kernel)");

	seq_puts(m, "\n   rows = page-table level,  cols = walk vs accounting\n");
	seq_puts(m, "   ------------------------------------------------------------\n");
	seq_printf(m, "   %-6s %14s %14s %14s\n",
		   "level", "master pages", "ring members", "pt_cur sum");
	for (level = MITOSIS_CACHE_PGD; level >= MITOSIS_CACHE_PTE; level--)
		seq_printf(m, "   %-6s %14ld %14ld %14ld\n",
			   lvl[level], r.pages[level], r.members[level],
			   r.pt_cur[level]);
	seq_puts(m, "   ------------------------------------------------------------\n");
	seq_puts(m, "   note: pt_cur may exceed ring members by off-tree THP-deposited\n");
	seq_puts(m, "         PTE pages (allocated un-replicated); that gap is expected.\n");
}
