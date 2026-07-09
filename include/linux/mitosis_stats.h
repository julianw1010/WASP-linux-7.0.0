#ifndef _LINUX_MITOSIS_STATS_H
#define _LINUX_MITOSIS_STATS_H

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/mm_types.h>

#define MITOSIS_PT_NR_LEVELS 5

struct mitosis_stats {
	struct list_head list;
	unsigned long id;
	int pid;
	char comm[TASK_COMM_LEN];
	void *mm;
	int master_node;
	int ever_enabled;

	atomic_long_t thp_split;
	atomic_long_t thp_collapse;
	atomic_long_t deposits;
	atomic_long_t withdrawals;

	atomic_long_t tlb_shootdowns;
	atomic_long_t tlb_broadcasts;

	atomic_long_t numa_migrate_4k[NUMA_NODE_COUNT][NUMA_NODE_COUNT];
	atomic_long_t numa_migrate_2m[NUMA_NODE_COUNT][NUMA_NODE_COUNT];

	atomic_long_t pt_cur[NUMA_NODE_COUNT][MITOSIS_PT_NR_LEVELS];
	atomic_long_t pt_max[NUMA_NODE_COUNT][MITOSIS_PT_NR_LEVELS];
};

struct mitosis_stats *mitosis_stats_birth(struct mm_struct *mm);
struct mitosis_stats *mitosis_stats_attach(struct mm_struct *mm, int master_node);
void mitosis_stats_retire(struct mm_struct *mm);
int mitosis_status_open(struct inode *inode, struct file *file);
int mitosis_history_open(struct inode *inode, struct file *file);

static inline void mitosis_stats_thp_split(struct mm_struct *mm)
{
	if (mm->mitosis_stats)
		atomic_long_inc(&mm->mitosis_stats->thp_split);
}

static inline void mitosis_stats_thp_collapse(struct mm_struct *mm)
{
	if (mm->mitosis_stats)
		atomic_long_inc(&mm->mitosis_stats->thp_collapse);
}

static inline void mitosis_stats_ring_account(struct page *pgtable, int delta)
{
	struct page *cur_page = pgtable;
	struct page *start_page = pgtable;

	do {
		mitosis_pt_account_page(cur_page, MITOSIS_CACHE_PTE, delta);
		cur_page = cur_page->pt_replica;
	} while (cur_page && cur_page != start_page);
}

static inline void mitosis_stats_deposit(struct mm_struct *mm, struct page *pgtable)
{
	if (mm->mitosis_stats)
		atomic_long_inc(&mm->mitosis_stats->deposits);

	mitosis_stats_ring_account(pgtable, -1);
}

static inline void mitosis_stats_withdraw(struct mm_struct *mm, struct page *pgtable)
{
	if (mm->mitosis_stats)
		atomic_long_inc(&mm->mitosis_stats->withdrawals);

	mitosis_stats_ring_account(pgtable, 1);
}

static inline void mitosis_stats_tlb(struct mm_struct *mm, long count)
{
	if (count > 0 && mm->mitosis_stats)
		atomic_long_add(count, &mm->mitosis_stats->tlb_shootdowns);
}

static inline void mitosis_stats_tlb_broadcast(struct mm_struct *mm, long count)
{
	if (count > 0 && mm->mitosis_stats)
		atomic_long_add(count, &mm->mitosis_stats->tlb_broadcasts);
}

static inline void mitosis_stats_numa(struct mm_struct *mm, bool huge,
				      int from, int to)
{
	struct mitosis_stats *s = mm->mitosis_stats;

	if (!s)
		return;
	if (from < 0 || from >= NUMA_NODE_COUNT ||
	    to < 0 || to >= NUMA_NODE_COUNT)
		return;
	if (huge)
		atomic_long_inc(&s->numa_migrate_2m[from][to]);
	else
		atomic_long_inc(&s->numa_migrate_4k[from][to]);
}

extern atomic_long_t mitosis_replica_allocs[MITOSIS_PT_NR_LEVELS];
extern atomic_long_t mitosis_replica_frees[MITOSIS_PT_NR_LEVELS];

static inline void mitosis_replica_alloc_inc(int level)
{
	if (level >= 0 && level < MITOSIS_PT_NR_LEVELS)
		atomic_long_inc(&mitosis_replica_allocs[level]);
}

static inline void mitosis_replica_free_inc(int level)
{
	if (level >= 0 && level < MITOSIS_PT_NR_LEVELS)
		atomic_long_inc(&mitosis_replica_frees[level]);
}

#endif
