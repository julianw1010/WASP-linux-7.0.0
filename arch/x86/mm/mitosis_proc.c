// SPDX-License-Identifier: GPL-2.0
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <asm/pgtable_repl.h>

static struct proc_dir_entry *mitosis_dir;

static int mitosis_cache_show(struct seq_file *m, void *v)
{
	int node;

	seq_printf(m, "%-6s %8s %12s %12s %12s\n",
		   "node", "count", "hits", "misses", "returns");

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct mitosis_cache_head *c = &mitosis_cache[node];

		seq_printf(m, "%-6d %8d %12lld %12lld %12lld\n",
			   node,
			   atomic_read(&c->count),
			   atomic64_read(&c->hits),
			   atomic64_read(&c->misses),
			   atomic64_read(&c->returns));
	}

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

	if (val <= 0 || val > 131072)
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

			if (!mitosis_cache_push(page, node, 0)) {
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

static int mitosis_verify_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_mitosis_verify_enabled);
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

	sysctl_mitosis_verify_enabled = (val != 0) ? 1 : 0;

	pr_info("MITOSIS: fault verification %s\n",
		sysctl_mitosis_verify_enabled ? "enabled" : "disabled");

	return count;
}

static const struct proc_ops mitosis_verify_ops = {
	.proc_open	= mitosis_verify_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_verify_write,
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

static ssize_t mitosis_inherit_write(struct file *file,
				     const char __user *ubuf,
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

static int mitosis_mode_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_mitosis_mode);
	return 0;
}

static int mitosis_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, mitosis_mode_show, NULL);
}

static ssize_t mitosis_mode_write(struct file *file, const char __user *ubuf,
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

	if (val > 1)
		val = 1;
	else if (val < -1)
		val = -1;

	sysctl_mitosis_mode = val;

	pr_info("MITOSIS: mode=%d\n", sysctl_mitosis_mode);

	return count;
}

static const struct proc_ops mitosis_mode_ops = {
	.proc_open	= mitosis_mode_open,
	.proc_read	= seq_read,
	.proc_write	= mitosis_mode_write,
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

	if (!proc_create("verify", 0644, mitosis_dir, &mitosis_verify_ops))
		goto fail;

	if (!proc_create("inherit", 0644, mitosis_dir, &mitosis_inherit_ops))
		goto fail;

	if (!proc_create("mode", 0644, mitosis_dir, &mitosis_mode_ops))
		goto fail;

	return 0;

fail:
	remove_proc_subtree("mitosis", NULL);
	return -ENOMEM;
}
late_initcall(mitosis_proc_init);
