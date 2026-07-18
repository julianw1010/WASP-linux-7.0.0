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
#include <linux/jiffies.h>
#include <linux/mitosis_stats.h>

static LIST_HEAD(mitosis_live_list);
static LIST_HEAD(mitosis_hist_list);
static DEFINE_SPINLOCK(mitosis_stats_lock);
static unsigned long mitosis_stats_next_id;

struct mitosis_stats *mitosis_stats_birth(struct mm_struct *mm)
{
	struct mitosis_stats *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;

	INIT_LIST_HEAD(&s->list);
	s->mm = mm;
	s->master_node = NUMA_NO_NODE;
	s->start_jiffies = jiffies;

	mm->mitosis_stats = s;
	return s;
}

struct mitosis_stats *mitosis_stats_attach(struct mm_struct *mm, int master_node)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s)
		s = mitosis_stats_birth(mm);
	if (!s)
		return NULL;

	s->pid = current->pid;
	get_task_comm(s->comm, current);
	s->master_node = master_node;

	if (!s->ever_enabled) {
		s->ever_enabled = 1;

		spin_lock(&mitosis_stats_lock);
		s->id = ++mitosis_stats_next_id;
		list_add_tail(&s->list, &mitosis_live_list);
		spin_unlock(&mitosis_stats_lock);
	}

	return s;
}

void mitosis_stats_stamp(struct mm_struct *mm, struct task_struct *tsk)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s || !s->ever_enabled)
		return;

	s->pid = task_pid_nr(tsk);
	get_task_comm(s->comm, tsk);
}

void mitosis_stats_publish(struct mm_struct *mm)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s || !s->ever_enabled)
		return;

	s->end_jiffies = jiffies;

	spin_lock(&mitosis_stats_lock);
	list_move_tail(&s->list, &mitosis_hist_list);
	spin_unlock(&mitosis_stats_lock);
}

void mitosis_stats_retire(struct mm_struct *mm)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s)
		return;

	mm->mitosis_stats = NULL;

	if (s->ever_enabled) {
		spin_lock(&mitosis_stats_lock);
		list_move_tail(&s->list, &mitosis_hist_list);
		spin_unlock(&mitosis_stats_lock);
	} else {
		kfree(s);
	}
}

int mitosis_stats_clear_history(void)
{
	struct mitosis_stats *s, *tmp;
	int freed = 0;

	spin_lock(&mitosis_stats_lock);
	list_for_each_entry_safe(s, tmp, &mitosis_hist_list, list) {
		list_del(&s->list);
		kfree(s);
		freed++;
	}
	spin_unlock(&mitosis_stats_lock);

	return freed;
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

void mitosis_stats_fault(struct mm_struct *mm, unsigned int flags)
{
	struct mitosis_stats *s;
	int node;

	if (!mm)
		return;

	s = mm->mitosis_stats;
	if (!s)
		return;

	atomic_long_inc(&s->faults);
	if (flags & FAULT_FLAG_WRITE)
		atomic_long_inc(&s->faults_write);
	if (flags & FAULT_FLAG_PROT)
		atomic_long_inc(&s->faults_present);

	node = numa_node_id();
	if (node >= 0 && node < NUMA_NODE_COUNT)
		atomic_long_inc(&s->faults_node[node]);
}

static bool mitosis_stats_ready __read_mostly;

static long mitosis_ring_len(struct page *base)
{
	struct page *cur = base->pt_replica;
	long n = 1;

	while (cur && cur != base) {
		n++;
		cur = cur->pt_replica;
	}
	return n;
}

void mitosis_stats_pt_write(void *tablep, int level)
{
	struct mm_struct *mm;
	struct mitosis_stats *s;
	struct page *base;

	if (!READ_ONCE(mitosis_stats_ready))
		return;
	if (level < 0 || level >= MITOSIS_PT_NR_LEVELS)
		return;
	if (!virt_addr_valid(tablep))
		return;

	base = virt_to_page(tablep);
	mm = READ_ONCE(base->pt_owner_mm);
	if (!mm)
		return;

	s = mm->mitosis_stats;
	if (s) {
		atomic_long_inc(&s->pt_writes[level]);
		atomic_long_add(mitosis_ring_len(base), &s->pt_pages[level]);
	}
}

static int __init mitosis_stats_init(void)
{
	WRITE_ONCE(mitosis_stats_ready, true);
	return 0;
}
early_initcall(mitosis_stats_init);

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

static void mitosis_print_group(struct seq_file *m, const char *name)
{
	seq_printf(m, "      %s:\n", name);
}

static void mitosis_print_sub2(struct seq_file *m, const char *label, long val)
{
	seq_printf(m, "        %-36s %12ld\n", label, val);
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

	{
		unsigned long endj = history ? s->end_jiffies : jiffies;
		unsigned int ms = jiffies_to_msecs(endj - s->start_jiffies);

		seq_printf(m, "    %-40s %u.%03u\n", "lifetime (s)",
			   ms / 1000, ms % 1000);
	}

	mitosis_print_section(m, "THP / page-table events");
	mitosis_print_kv(m, "THP splits", atomic_long_read(&s->thp_split));
	mitosis_print_kv(m, "THP collapses", atomic_long_read(&s->thp_collapse));
	mitosis_print_kv(m, "THP pgtable deposits",
			 atomic_long_read(&s->deposits));
	mitosis_print_kv(m, "THP pgtable withdrawals",
			 atomic_long_read(&s->withdrawals));

	{
		long f = atomic_long_read(&s->faults);
		long fw = atomic_long_read(&s->faults_write);
		long fp = atomic_long_read(&s->faults_present);
		int node;

		mitosis_print_section(m, "Page faults");
		seq_puts(m,
			 "  (each of the two breakdowns below sums to the total independently)\n");
		mitosis_print_kv(m, "Total faults", f);
		mitosis_print_group(m, "by access");
		mitosis_print_sub2(m, "read", f - fw);
		mitosis_print_sub2(m, "write", fw);
		mitosis_print_group(m, "by fault type");
		mitosis_print_sub2(m, "not-present (major/fill)", f - fp);
		mitosis_print_sub2(m, "present (permission/minor)", fp);
		mitosis_print_group(m, "by handling node  [cols = NUMA node]");
		mitosis_print_node_header(m);
		seq_printf(m, "    %-4s", "flts");
		for (node = 0; node < NUMA_NODE_COUNT; node++)
			seq_printf(m, " %7ld",
				   atomic_long_read(&s->faults_node[node]));
		seq_putc(m, '\n');
	}

	{
		char buf[24];
		int lvl;

		mitosis_print_section(m,
			"Page-table entry modifications + replica fan-out (all ops)  [rows = level]");
		seq_puts(m,
			 "  (writes = set/clear/wrprotect/young calls; pages = replica table pages touched)\n");
		seq_printf(m, "      %-6s %14s %14s %16s\n",
			   "level", "writes", "pages", "avg pages/write");
		for (lvl = MITOSIS_CACHE_PGD; lvl >= MITOSIS_CACHE_PTE; lvl--) {
			long writes = atomic_long_read(&s->pt_writes[lvl]);
			long pages = atomic_long_read(&s->pt_pages[lvl]);
			long h = writes ? (pages * 100 + writes / 2) / writes : 0;

			scnprintf(buf, sizeof(buf), "%ld.%02ld", h / 100, h % 100);
			seq_printf(m, "      %-6s %14ld %14ld %16s\n",
				   mitosis_level_name[lvl], writes, pages, buf);
		}
	}

	mitosis_print_section(m, "TLB shootdowns (remote-CPU IPIs)");
	mitosis_print_kv(m, "Total shootdowns",
			 atomic_long_read(&s->tlb_shootdowns));

	mitosis_print_section(m, "TLB broadcasts (INVLPGB, no IPIs)");
	mitosis_print_kv(m, "Total INVLPGB instructions",
			 atomic_long_read(&s->tlb_broadcasts));

	mitosis_print_section(m,
		"autoNUMA migrations: 4KB base pages  [rows = source node, cols = dest node]");
	mitosis_print_node_matrix(m, s->numa_migrate_4k);

	mitosis_print_section(m,
		"autoNUMA migrations: 2MB THP pages  [rows = source node, cols = dest node]");
	mitosis_print_node_matrix(m, s->numa_migrate_2m);

	mitosis_print_section(m, history ?
		"Page tables (master + replicas): MAX ever existed  [rows = level, cols = node]" :
		"Page tables (master + replicas): current  [rows = level, cols = node]");
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
