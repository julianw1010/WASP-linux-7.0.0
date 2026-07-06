#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <asm/mitosis.h>
#include <linux/mitosis_stats.h>

static struct proc_dir_entry *mitosis_dir;

static void mitosis_cache_size_cell(struct seq_file *m, long long pages)
{
	char buf[24];
	long long tenths = pages * 5 / 128;

	scnprintf(buf, sizeof(buf), "%lld.%lld", tenths / 10, tenths % 10);
	seq_printf(m, " %10s", buf);
}

static int mitosis_cache_show(struct seq_file *m, void *v)
{
	char buf[12];
	long long tot_pages = 0, tot_hits = 0, tot_misses = 0, tot_returns = 0;
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		tot_pages += atomic_read(&mitosis_cache[node].count);
		tot_hits += atomic64_read(&mitosis_cache[node].hits);
		tot_misses += atomic64_read(&mitosis_cache[node].misses);
		tot_returns += atomic64_read(&mitosis_cache[node].returns);
	}

	seq_puts(m, " Mitosis per-node page-table page cache\n");
	seq_puts(m, " write N > 0: add N pages to the cache of every online node\n");
	seq_puts(m, " write -1:    drain all nodes\n");
	seq_puts(m, " rows = cache metric,  cols = NUMA node\n");
	seq_puts(m, " --------------------------------------------------------------------------------------------------------------\n");

	seq_printf(m, " %-10s", "");
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		scnprintf(buf, sizeof(buf), "n%d", node);
		seq_printf(m, " %10s", buf);
	}
	seq_printf(m, " %10s\n", "TOTAL");

	seq_printf(m, " %-10s", "pages");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10d", atomic_read(&mitosis_cache[node].count));
	seq_printf(m, " %10lld\n", tot_pages);

	seq_printf(m, " %-10s", "size (MiB)");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		mitosis_cache_size_cell(m, atomic_read(&mitosis_cache[node].count));
	mitosis_cache_size_cell(m, tot_pages);
	seq_putc(m, '\n');

	seq_printf(m, " %-10s", "hits");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10lld", atomic64_read(&mitosis_cache[node].hits));
	seq_printf(m, " %10lld\n", tot_hits);

	seq_printf(m, " %-10s", "misses");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10lld", atomic64_read(&mitosis_cache[node].misses));
	seq_printf(m, " %10lld\n", tot_misses);

	seq_printf(m, " %-10s", "returns");
	for (node = 0; node < NUMA_NODE_COUNT; node++)
		seq_printf(m, " %10lld", atomic64_read(&mitosis_cache[node].returns));
	seq_printf(m, " %10lld\n", tot_returns);

	return 0;
}

static int mitosis_cache_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_cache_show, NULL);
}

static ssize_t mitosis_cache_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;
	int node, added, total, drained;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		drained = mitosis_cache_drain_all();
		pr_info("MITOSIS: cache drained %d pages\n", drained);
		return count;
	}

	if (val <= 0)
		return -EINVAL;

	total = 0;
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		if (!node_online(node))
			continue;

		added = 0;
		while (added < val) {
			struct page *page;

			page = alloc_pages_node(node,
				GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
			if (!page)
				break;

			if (page_to_nid(page) != node) {
				__free_page(page);
				break;
			}

			page->pt_replica = NULL;
			page->pt_owner_mm = NULL;

			if (!mitosis_cache_push(page, node, 0, NULL)) {
				__free_page(page);
				break;
			}

			added++;
		}

		total += added;
	}

	pr_info("MITOSIS: cache populated %d pages across %d nodes\n",
		total, num_online_nodes());

	return count;
}

static const struct proc_ops mitosis_cache_ops = {
	.proc_open	= mitosis_cache_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_cache_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int mitosis_inherit_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_mitosis_inherit);
	return 0;
}

static int mitosis_inherit_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_inherit_show, NULL);
}

static ssize_t mitosis_inherit_write(struct file *file, const char __user *ubuf,
				     size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	sysctl_mitosis_inherit = (val > 0) ? 1 : -1;

	pr_info("MITOSIS: inheritance %s\n",
		sysctl_mitosis_inherit == 1 ? "enabled" : "disabled");

	return count;
}

static const struct proc_ops mitosis_inherit_ops = {
	.proc_open	= mitosis_inherit_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_inherit_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int mitosis_verify_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", READ_ONCE(mitosis_verify));
	return 0;
}

static int mitosis_verify_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_verify_show, NULL);
}

static ssize_t mitosis_verify_write(struct file *file, const char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	WRITE_ONCE(mitosis_verify, val ? 1 : 0);

	pr_info("MITOSIS: verify %s\n",
		READ_ONCE(mitosis_verify) ? "enabled" : "disabled");

	return count;
}

static const struct proc_ops mitosis_verify_ops = {
	.proc_open	= mitosis_verify_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_verify_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops mitosis_status_ops = {
	.proc_open	= mitosis_status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static const struct proc_ops mitosis_history_ops = {
	.proc_open	= mitosis_history_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int mitosis_balance_show(struct seq_file *m, void *v)
{
	static const char * const lvl[MITOSIS_PT_NR_LEVELS] = {
		[MITOSIS_CACHE_PTE] = "PTE",
		[MITOSIS_CACHE_PMD] = "PMD",
		[MITOSIS_CACHE_PUD] = "PUD",
		[MITOSIS_CACHE_P4D] = "P4D",
		[MITOSIS_CACHE_PGD] = "PGD",
	};
	long total_alloc = 0, total_free = 0;
	int level;

	seq_puts(m, " Mitosis replica-page balance  (excess copies only; primary NOT counted)\n");
	seq_puts(m, " rows = page-table level,  cols = cumulative count since boot\n");
	seq_puts(m, " LIVE = ALLOCS - FREES;  must read 0 when no replicated process is alive\n");
	seq_puts(m, " -----------------------------------------------------------------------\n");
	seq_printf(m, " %-6s %15s %15s %15s\n", "level", "allocs", "frees", "live");

	for (level = MITOSIS_CACHE_PTE; level <= MITOSIS_CACHE_PGD; level++) {
		long a = atomic_long_read(&mitosis_replica_allocs[level]);
		long f = atomic_long_read(&mitosis_replica_frees[level]);

		total_alloc += a;
		total_free += f;
		seq_printf(m, " %-6s %15ld %15ld %15ld\n", lvl[level], a, f, a - f);
	}

	seq_puts(m, " -----------------------------------------------------------------------\n");
	seq_printf(m, " %-6s %15ld %15ld %15ld\n", "TOTAL",
		   total_alloc, total_free, total_alloc - total_free);
	return 0;
}

static int mitosis_balance_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_balance_show, NULL);
}

static const struct proc_ops mitosis_balance_ops = {
	.proc_open	= mitosis_balance_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int mitosis_audit_show(struct seq_file *m, void *v)
{
	mitosis_audit_seq_show(m);
	return 0;
}

static int mitosis_audit_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_audit_show, NULL);
}

static ssize_t mitosis_audit_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long pid;
	int ret;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &pid))
		return -EINVAL;
	if (pid < 0)
		return -EINVAL;

	ret = mitosis_audit_run((pid_t)pid);
	if (ret)
		return ret;

	return count;
}

static const struct proc_ops mitosis_audit_ops = {
	.proc_open	= mitosis_audit_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_audit_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init mitosis_proc_init(void)
{
	mitosis_dir = proc_mkdir("mitosis", NULL);
	if (!mitosis_dir)
		return -ENOMEM;

	if (!proc_create("cache", 0644, mitosis_dir, &mitosis_cache_ops))
		goto fail;

	if (!proc_create("inherit", 0644, mitosis_dir, &mitosis_inherit_ops))
		goto fail;

	if (!proc_create("verify", 0644, mitosis_dir, &mitosis_verify_ops))
		goto fail;

	if (!proc_create("status", 0444, mitosis_dir, &mitosis_status_ops))
		goto fail;

	if (!proc_create("history", 0444, mitosis_dir, &mitosis_history_ops))
		goto fail;

	if (!proc_create("balance", 0444, mitosis_dir, &mitosis_balance_ops))
		goto fail;

	if (!proc_create("audit", 0644, mitosis_dir, &mitosis_audit_ops))
		goto fail;
	return 0;

fail:
	remove_proc_subtree("mitosis", NULL);
	return -ENOMEM;
}
late_initcall(mitosis_proc_init);
