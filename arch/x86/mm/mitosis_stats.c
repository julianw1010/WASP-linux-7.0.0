#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/mitosis.h>
#include <linux/mitosis_stats.h>

static LIST_HEAD(mitosis_live_list);
static LIST_HEAD(mitosis_hist_list);
static DEFINE_SPINLOCK(mitosis_stats_lock);
static unsigned long mitosis_stats_next_id;

struct mitosis_stats *mitosis_stats_attach(struct mm_struct *mm, int master_node)
{
	struct mitosis_stats *s;

	if (mm->mitosis_stats)
		return mm->mitosis_stats;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->list);
	s->pid = current->pid;
	get_task_comm(s->comm, current);
	s->mm = mm;
	s->master_node = master_node;

	spin_lock(&mitosis_stats_lock);
	s->id = ++mitosis_stats_next_id;
	list_add_tail(&s->list, &mitosis_live_list);
	spin_unlock(&mitosis_stats_lock);

	mm->mitosis_stats = s;
	return s;
}

void mitosis_stats_to_history(struct mm_struct *mm)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s)
		return;

	mm->mitosis_stats = NULL;

	spin_lock(&mitosis_stats_lock);
	list_move_tail(&s->list, &mitosis_hist_list);
	spin_unlock(&mitosis_stats_lock);
}

static void mitosis_bump_max(atomic_long_t *maxp, long cur)
{
	long mx = atomic_long_read(maxp);

	while (cur > mx) {
		long prev = atomic_long_cmpxchg(maxp, mx, cur);

		if (prev == mx)
			break;
		mx = prev;
	}
}

void mitosis_pt_account_mm(struct mm_struct *mm, int node, int level, int delta)
{
	struct mitosis_stats *s;

	if (!mm)
		return;

	s = mm->mitosis_stats;
	if (!s)
		return;

	if (level < 0 || level >= MITOSIS_PT_NR_LEVELS)
		return;
	if (node < 0 || node >= NUMA_NODE_COUNT)
		return;

	if (delta > 0)
		mitosis_bump_max(&s->pt_max[node][level],
				 atomic_long_inc_return(&s->pt_cur[node][level]));
	else
		atomic_long_dec(&s->pt_cur[node][level]);
}

void mitosis_pt_account_page(struct page *page, int level, int delta)
{
	if (!page)
		return;

	mitosis_pt_account_mm(page->pt_owner_mm, page_to_nid(page), level, delta);
}

void mitosis_stats_seed(struct mm_struct *mm)
{
	unsigned long addr, end = TASK_SIZE;
	unsigned long next_pgd, next_p4d, next_pud, next_pmd;
	pgd_t *pgd_base = mm->pgd;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (!mm->mitosis_stats)
		return;

	mitosis_pt_account_mm(mm, page_to_nid(virt_to_page(pgd_base)),
			      MITOSIS_CACHE_PGD, 1);

	addr = 0;
	pgd = pgd_offset_pgd(pgd_base, addr);
	do {
		next_pgd = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto next_pgd_seed;

		p4d = p4d_offset(pgd, addr);
		if (virt_to_page(p4d) != virt_to_page(pgd_base))
			mitosis_pt_account_mm(mm, page_to_nid(virt_to_page(p4d)),
					      MITOSIS_CACHE_P4D, 1);
		do {
			next_p4d = p4d_addr_end(addr, next_pgd);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				goto next_p4d_seed;

			pud = pud_offset(p4d, addr);
			mitosis_pt_account_mm(mm, page_to_nid(virt_to_page(pud)),
					      MITOSIS_CACHE_PUD, 1);
			do {
				next_pud = pud_addr_end(addr, next_p4d);
				if (pud_none(*pud) || pud_bad(*pud) ||
				    pud_leaf(*pud))
					goto next_pud_seed;

				pmd = pmd_offset(pud, addr);
				mitosis_pt_account_mm(mm,
						      page_to_nid(virt_to_page(pmd)),
						      MITOSIS_CACHE_PMD, 1);
				do {
					next_pmd = pmd_addr_end(addr, next_pud);
					if (pmd_none(*pmd) || pmd_bad(*pmd) ||
					    pmd_trans_huge(*pmd) ||
					    pmd_leaf(*pmd))
						goto next_pmd_seed;

					pte = pte_offset_kernel(pmd, addr);
					mitosis_pt_account_mm(mm,
							      page_to_nid(virt_to_page(pte)),
							      MITOSIS_CACHE_PTE, 1);
next_pmd_seed:
					addr = next_pmd;
				} while (pmd++, addr != next_pud);
next_pud_seed:
				addr = next_pud;
			} while (pud++, addr != next_p4d);
next_p4d_seed:
			addr = next_p4d;
		} while (p4d++, addr != next_pgd);
next_pgd_seed:
		addr = next_pgd;
	} while (pgd++, addr != end);
}

static const char * const mitosis_level_name[MITOSIS_PT_NR_LEVELS] = {
	[MITOSIS_CACHE_PTE] = "PTE",
	[MITOSIS_CACHE_PMD] = "PMD",
	[MITOSIS_CACHE_PUD] = "PUD",
	[MITOSIS_CACHE_P4D] = "P4D",
	[MITOSIS_CACHE_PGD] = "PGD",
};

#define MITOSIS_RULE \
	"========================================================================"

static void mitosis_print_section(struct seq_file *m, const char *name)
{
	seq_printf(m, "\n  %s\n", name);
	seq_puts(m, "  ------------------------------------------------------------------\n");
}

static void mitosis_print_kv(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "    %-40s %12ld\n", label, val);
}

static void mitosis_print_node_header(struct seq_file *m)
{
	char buf[12];
	int n;

	seq_puts(m, "        ");
	for (n = 0; n < NUMA_NODE_COUNT; n++) {
		scnprintf(buf, sizeof(buf), "n%d", n);
		seq_printf(m, " %7s", buf);
	}
	seq_putc(m, '\n');
}

static void mitosis_print_node_matrix(struct seq_file *m,
				      atomic_long_t mat[NUMA_NODE_COUNT][NUMA_NODE_COUNT])
{
	char buf[12];
	int from, to;

	mitosis_print_node_header(m);
	for (from = 0; from < NUMA_NODE_COUNT; from++) {
		scnprintf(buf, sizeof(buf), "n%d", from);
		seq_printf(m, "    %-4s", buf);
		for (to = 0; to < NUMA_NODE_COUNT; to++)
			seq_printf(m, " %7ld", atomic_long_read(&mat[from][to]));
		seq_putc(m, '\n');
	}
}

static void mitosis_print_pt_table(struct seq_file *m, struct mitosis_stats *s,
				   bool history)
{
	int node, lvl;

	mitosis_print_node_header(m);
	for (lvl = MITOSIS_CACHE_PGD; lvl >= MITOSIS_CACHE_PTE; lvl--) {
		seq_printf(m, "    %-4s", mitosis_level_name[lvl]);
		for (node = 0; node < NUMA_NODE_COUNT; node++)
			seq_printf(m, " %7ld",
				   atomic_long_read(history ?
						    &s->pt_max[node][lvl] :
						    &s->pt_cur[node][lvl]));
		seq_putc(m, '\n');
	}
}

static void mitosis_stats_print(struct seq_file *m, struct mitosis_stats *s,
				bool history)
{
	seq_printf(m, "%s\n", MITOSIS_RULE);
	seq_printf(m, "  MM record #%lu\n", s->id);
	seq_printf(m, "%s\n", MITOSIS_RULE);
	seq_printf(m, "    %-40s %d\n", "pid", s->pid);
	seq_printf(m, "    %-40s %s\n", "comm", s->comm);
	seq_printf(m, "    %-40s %px\n", "mm", s->mm);
	seq_printf(m, "    %-40s %d\n", "master node", s->master_node);

	mitosis_print_section(m, "THP / page-table events");
	mitosis_print_kv(m, "THP splits", atomic_long_read(&s->thp_split));
	mitosis_print_kv(m, "THP collapses", atomic_long_read(&s->thp_collapse));
	mitosis_print_kv(m, "THP pgtable deposits",
			 atomic_long_read(&s->deposits));
	mitosis_print_kv(m, "THP pgtable withdrawals",
			 atomic_long_read(&s->withdrawals));

	mitosis_print_section(m, "TLB shootdowns (remote-CPU IPIs)");
	mitosis_print_kv(m, "Total shootdowns",
			 atomic_long_read(&s->tlb_shootdowns));

	mitosis_print_section(m,
		"autoNUMA migrations: 4KB base pages  [rows = source node, cols = dest node]");
	mitosis_print_node_matrix(m, s->numa_migrate_4k);

	mitosis_print_section(m,
		"autoNUMA migrations: 2MB THP pages  [rows = source node, cols = dest node]");
	mitosis_print_node_matrix(m, s->numa_migrate_2m);

	mitosis_print_section(m, history ?
		"Page-table replicas: MAX watermark  [rows = level, cols = node]" :
		"Page-table replicas: current  [rows = level, cols = node]");
	mitosis_print_pt_table(m, s, history);

	seq_putc(m, '\n');
}

static void *mitosis_live_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&mitosis_stats_lock);
	m->private = &mitosis_live_list;
	return seq_list_start(&mitosis_live_list, *pos);
}

static void *mitosis_hist_start(struct seq_file *m, loff_t *pos)
{
	spin_lock(&mitosis_stats_lock);
	m->private = &mitosis_hist_list;
	return seq_list_start(&mitosis_hist_list, *pos);
}

static void *mitosis_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, (struct list_head *)m->private, pos);
}

static void mitosis_seq_stop(struct seq_file *m, void *v)
{
	spin_unlock(&mitosis_stats_lock);
}

static int mitosis_seq_show(struct seq_file *m, void *v)
{
	struct mitosis_stats *s = list_entry(v, struct mitosis_stats, list);

	mitosis_stats_print(m, s, m->private == &mitosis_hist_list);
	return 0;
}

static const struct seq_operations mitosis_live_seq_ops = {
	.start	= mitosis_live_start,
	.next	= mitosis_seq_next,
	.stop	= mitosis_seq_stop,
	.show	= mitosis_seq_show,
};

static const struct seq_operations mitosis_hist_seq_ops = {
	.start	= mitosis_hist_start,
	.next	= mitosis_seq_next,
	.stop	= mitosis_seq_stop,
	.show	= mitosis_seq_show,
};

int mitosis_status_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &mitosis_live_seq_ops);
}

int mitosis_history_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &mitosis_hist_seq_ops);
}
