/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/compaction.h>
#include <linux/fs.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#ifdef CONFIG_ZRAM_FOR_ANDROID
#include <linux/device.h>
#include <linux/err.h>
#include <linux/mm_inline.h>
#endif /* CONFIG_ZRAM_FOR_ANDROID */
#define ENHANCED_LMK_ROUTINE

#ifdef ENHANCED_LMK_ROUTINE
#define LOWMEM_DEATHPENDING_DEPTH 3
#endif

static uint32_t lowmem_debug_level = 1;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
static int lmk_fast_run = 1;
#ifdef CONFIG_ZRAM_FOR_ANDROID
static struct class *lmk_class;
static struct device *lmk_dev;
static int lmk_kill_pid = 0;
static int lmk_kill_ok = 0;

extern atomic_t optimize_comp_on;

extern int isolate_lru_page_compcache(struct page *page);
extern void putback_lru_page(struct page *page);
extern unsigned int zone_id_shrink_pagelist(struct zone *zone_id,struct list_head *page_list);

#define lru_to_page(_head) (list_entry((_head)->prev, struct page, lru))

#define SWAP_PROCESS_DEBUG_LOG 0
/* free RAM 8M(2048 pages) */
#define CHECK_FREE_MEMORY 2048
/* free swap (10240 pages) */
#define CHECK_FREE_SWAPSPACE  10240

static unsigned int check_free_memory = 0;

enum pageout_io {
	PAGEOUT_IO_ASYNC,
	PAGEOUT_IO_SYNC,
};


#endif /* CONFIG_ZRAM_FOR_ANDROID */

#ifdef ENHANCED_LMK_ROUTINE
static struct task_struct *lowmem_deathpending[LOWMEM_DEATHPENDING_DEPTH] = {NULL,};
#else
static unsigned int offlining;
#endif
static unsigned long lowmem_deathpending_timeout;

extern int compact_nodes(bool sync);

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

#ifdef CONFIG_MEMORY_HOTPLUG
static int lmk_hotplug_callback(struct notifier_block *self,
				unsigned long cmd, void *data)
{
	switch (cmd) {
	/* Don't care LMK cases */
	case MEM_ONLINE:
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
	case MEM_CANCEL_OFFLINE:
	case MEM_GOING_ONLINE:
		offlining = 0;
		lowmem_print(4, "lmk in normal mode\n");
		break;
	/* LMK should account for movable zone */
	case MEM_GOING_OFFLINE:
		offlining = 1;
		lowmem_print(4, "lmk in hotplug mode\n");
		break;
	}
	return NOTIFY_DONE;
}
#endif

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}

static DEFINE_MUTEX(scan_mutex);

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		if ((zone_idx = zonelist_zone_idx(zoneref)) == ZONE_MOVABLE)
			continue;

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
							       NR_FILE_PAGES)
					      - zone_page_state(zone, NR_SHMEM)
						  - zone_page_state(zone, NR_SWAPCACHE);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0))
				*other_free -=
				           zone->lowmem_reserve[classzone_idx];
			else
				*other_free -=
				           zone_page_state(zone, NR_FREE_PAGES);
		}
	}
}

#ifdef CONFIG_HIGHMEM
void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
				&preferred_zone);

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0,
					0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	first_zones_zonelist(zonelist, high_zoneidx, NULL, &preferred_zone);
	classzone_idx = zone_idx(preferred_zone);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
			   KSWAPD_ZONE_BALANCE_GAP_RATIO);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0))
			*other_free -=
			           preferred_zone->lowmem_reserve[_ZONE];
		else
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file);

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
#ifdef ENHANCED_LMK_ROUTINE
	struct task_struct *selected[LOWMEM_DEATHPENDING_DEPTH] = {NULL,};
#else
	struct task_struct *selected = NULL;
#endif
	int rem = 0;
	int tasksize;
	int i;
	int min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
#ifdef ENHANCED_LMK_ROUTINE
	int selected_tasksize[LOWMEM_DEATHPENDING_DEPTH] = {0,};
	int selected_oom_adj[LOWMEM_DEATHPENDING_DEPTH] = {OOM_ADJUST_MAX,};
	int all_selected_oom = 0;
	int max_selected_oom_idx = 0;
#else
	int selected_tasksize = 0;
	int selected_oom_score_adj;
#endif
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	unsigned long nr_to_scan = sc->nr_to_scan;
	struct zone *zone;

	if (nr_to_scan > 0) {
		if (mutex_lock_interruptible(&scan_mutex) < 0)
			return 0;
	}

	other_free = global_page_state(NR_FREE_PAGES);

	if (global_page_state(NR_SHMEM) + total_swapcache_pages <
		global_page_state(NR_FILE_PAGES))
		other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM) -
						total_swapcache_pages;
	else
		other_file = 0;

		if (offlining) {
		/* Discount all free space in the section being offlined */
		for_each_zone(zone) {
			 if (zone_idx(zone) == ZONE_MOVABLE) {
				other_free -= zone_page_state(zone,
						NR_FREE_PAGES);
				lowmem_print(4, "lowmem_shrink discounted "
					"%lu pages in movable zone\n",
					zone_page_state(zone, NR_FREE_PAGES));
			}
		}
	}

	tune_lmk_param(&other_free, &other_file, sc);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}
	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %lu, %x, ofree %d %d, ma %d\n",
				nr_to_scan, sc->gfp_mask, other_free,
				other_file, min_score_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0 || min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %lu, %x, return %d\n",
			     nr_to_scan, sc->gfp_mask, rem);

		if (nr_to_scan > 0)
			mutex_unlock(&scan_mutex);

		return rem;
	}

#ifdef ENHANCED_LMK_ROUTINE
	for (i = 0; i < LOWMEM_DEATHPENDING_DEPTH; i++)
		selected_oom_adj[i] = min_adj;
#else
	selected_oom_score_adj = min_score_adj;
#endif

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		int oom_score_adj;
#ifdef ENHANCED_LMK_ROUTINE
		int is_exist_oom_task = 0;
#endif
		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			if (test_task_flag(tsk, TIF_MEMDIE)) {
				rcu_read_unlock();
				/* give the system time to free up the memory */
				msleep_interruptible(20);
				mutex_unlock(&scan_mutex);
				return 0;
			}
		}

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;

#ifdef ENHANCED_LMK_ROUTINE
		if (all_selected_oom < LOWMEM_DEATHPENDING_DEPTH) {
			for (i = 0; i < LOWMEM_DEATHPENDING_DEPTH; i++) {
				if (!selected[i]) {
					is_exist_oom_task = 1;
					max_selected_oom_idx = i;
					break;
				}
			}
		} else if (selected_oom_adj[max_selected_oom_idx] < oom_adj ||
			(selected_oom_adj[max_selected_oom_idx] == oom_adj &&
			selected_tasksize[max_selected_oom_idx] < tasksize)) {
			is_exist_oom_task = 1;
		}

		if (is_exist_oom_task) {
			selected[max_selected_oom_idx] = p;
			selected_tasksize[max_selected_oom_idx] = tasksize;
			selected_oom_adj[max_selected_oom_idx] = oom_adj;

			if (all_selected_oom < LOWMEM_DEATHPENDING_DEPTH)
				all_selected_oom++;

			if (all_selected_oom == LOWMEM_DEATHPENDING_DEPTH) {
				for (i = 0; i < LOWMEM_DEATHPENDING_DEPTH; i++) {
					if (selected_oom_adj[i] < selected_oom_adj[max_selected_oom_idx])
						max_selected_oom_idx = i;
					else if (selected_oom_adj[i] == selected_oom_adj[max_selected_oom_idx] &&
						selected_tasksize[i] < selected_tasksize[max_selected_oom_idx])
						max_selected_oom_idx = i;
				}
			}

			lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
				p->pid, p->comm, oom_adj, tasksize);
		}
#else
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_score_adj = oom_score_adj;
		lowmem_print(2, "select '%s' (%d), adj %d, size %d, to kill\n",
			    p->comm, p->pid, oom_score_adj, tasksize);
#endif
	}
#ifdef ENHANCED_LMK_ROUTINE
	for (i = 0; i < LOWMEM_DEATHPENDING_DEPTH; i++) {
		if (selected[i]) {
			lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
				selected[i]->pid, selected[i]->comm,
				selected_oom_adj[i], selected_tasksize[i]);
			lowmem_deathpending[i] = selected[i];
			lowmem_deathpending_timeout = jiffies + HZ;
			force_sig(SIGKILL, selected[i]);
			rem -= selected_tasksize[i];
		}
	}
#else
	if (selected) {
		lowmem_print(1, "Killing '%s' (%d), adj %d,\n" \
			     "   to free %ldkB on behalf of '%s' (%d) because\n" \
			     "   cache %ldkB is below limit %ldkB for oom_score_adj %d\n" \
			     "   Free memory is %ldkB above reserved\n",
			selected->comm, selected->pid,
			selected_oom_score_adj,
			selected_tasksize * (long)(PAGE_SIZE / 1024),
			current->comm, current->pid,
			other_file * (long)(PAGE_SIZE / 1024),
			minfree * (long)(PAGE_SIZE / 1024),
			min_score_adj,
			other_free * (long)(PAGE_SIZE / 1024));
		lowmem_deathpending_timeout = jiffies + HZ;
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
		rcu_read_unlock();
		/* give the system time to free up the memory */
		msleep_interruptible(20);
	} else
		rcu_read_unlock();
#endif
	lowmem_print(4, "lowmem_shrink %lu, %x, return %d\n",
		     nr_to_scan, sc->gfp_mask, rem);
	mutex_unlock(&scan_mutex);
	if (selected)
		compact_nodes(false);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

#ifdef CONFIG_ZRAM_FOR_ANDROID
/*
 * zone_id_shrink_pagelist() clear page flags,
 * update the memory zone status, and swap pagelist
 */

static unsigned int shrink_pages(struct mm_struct *mm,
				 struct list_head *zone0_page_list,
				 struct list_head *zone1_page_list,
				 unsigned int num_to_scan)
{
	unsigned long addr;
	unsigned int isolate_pages_countter = 0;

	struct vm_area_struct *vma = mm->mmap;
	while (vma != NULL) {

		for (addr = vma->vm_start; addr < vma->vm_end;
		     addr += PAGE_SIZE) {
			struct page *page;
			/*get the page address from virtual memory address */
			page = follow_page(vma, addr, FOLL_GET);

			if (page && !IS_ERR(page)) {

				put_page(page);
				/* only moveable, anonymous and not dirty pages can be swapped  */
				if ((!PageUnevictable(page))
				    && (!PageDirty(page)) && ((PageAnon(page)))
				    && (0 == page_is_file_cache(page))) {
					switch (page_zone_id(page)) {
					case 0:
						if (!isolate_lru_page_compcache(page)) {
							/* isolate page from LRU and add to temp list  */
							/*create new page list, it will be used in shrink_page_list */
							list_add_tail(&page->lru, zone0_page_list);
							isolate_pages_countter++;
						}
						break;
					case 1:
						if (!isolate_lru_page_compcache(page)) {
							/* isolate page from LRU and add to temp list  */
							/*create new page list, it will be used in shrink_page_list */
							list_add_tail(&page->lru, zone1_page_list);
							isolate_pages_countter++;
						}
						break;
					default:
						break;
					}
				}
			}

			if (isolate_pages_countter >= num_to_scan) {
				return isolate_pages_countter;
			}
		}

		vma = vma->vm_next;
	}

	return isolate_pages_countter;
}

/*
 * swap_application_pages() will search the
 * pages which can be swapped, then call
 * zone_id_shrink_pagelist to update zone
 * status
 */
static unsigned int swap_pages(struct list_head *zone0_page_list,
			       struct list_head *zone1_page_list)
{
	struct zone *zone_id_0 = &NODE_DATA(0)->node_zones[0];
	struct zone *zone_id_1 = &NODE_DATA(0)->node_zones[1];
	unsigned int pages_counter = 0;

	/*if the page list is not empty, call zone_id_shrink_pagelist to update zone status */
	if ((zone_id_0) && (!list_empty(zone0_page_list))) {
		pages_counter +=
		    zone_id_shrink_pagelist(zone_id_0, zone0_page_list);
	}
	if ((zone_id_1) && (!list_empty(zone1_page_list))) {
		pages_counter +=
		    zone_id_shrink_pagelist(zone_id_1, zone1_page_list);
	}
	return pages_counter;
}

static ssize_t lmk_state_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d,%d\n", lmk_kill_pid, lmk_kill_ok);
}

/*
 * lmk_state_store() will called by framework,
 * the framework will send the pid of process that need to be swapped
 */
static ssize_t lmk_state_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t size)
{
	sscanf(buf, "%d,%d", &lmk_kill_pid, &lmk_kill_ok);

	/* if the screen on, the optimized compcache will stop */
	if (atomic_read(&optimize_comp_on) != 1)
		return size;

	if (lmk_kill_ok == 1) {
		struct task_struct *p;
		struct task_struct *selected = NULL;
		struct sysinfo ramzswap_info = { 0 };
		struct mm_struct *mm_scan = NULL;

		/*
		 * check the free RAM and swap area,
		 * stop the optimized compcache in cpu idle case;
		 * leave some swap area for using in low memory case
		 */
		si_swapinfo(&ramzswap_info);
		si_meminfo(&ramzswap_info);

		if ((ramzswap_info.freeswap < CHECK_FREE_SWAPSPACE) ||
		    (ramzswap_info.freeram < check_free_memory)) {
#if SWAP_PROCESS_DEBUG_LOG > 0
			printk(KERN_INFO "idletime compcache is ignored : free RAM %lu, free swap %lu\n",
			ramzswap_info.freeram, ramzswap_info.freeswap);
#endif
			lmk_kill_ok = 0;
			return size;
		}

		read_lock(&tasklist_lock);
		for_each_process(p) {
			if ((p->pid == lmk_kill_pid) &&
			    (__task_cred(p)->uid > 10000)) {
				task_lock(p);
				selected = p;
				if (!selected->mm || !selected->signal) {
					task_unlock(p);
					selected = NULL;
					break;
				}
				mm_scan = selected->mm;
				if (mm_scan) {
					if (selected->flags & PF_KTHREAD)
						mm_scan = NULL;
					else
						atomic_inc(&mm_scan->mm_users);
				}
				task_unlock(selected);

#if SWAP_PROCESS_DEBUG_LOG > 0
				printk(KERN_INFO "idle time compcache: swap process pid %d, name %s, oom %d, task size %ld\n",
					p->pid, p->comm,
					p->signal->oom_adj,
					get_mm_rss(p->mm));
#endif
				break;
			}
		}
		read_unlock(&tasklist_lock);

		if (mm_scan) {
			LIST_HEAD(zone0_page_list);
			LIST_HEAD(zone1_page_list);
			int pages_tofree = 0, pages_freed = 0;

			down_read(&mm_scan->mmap_sem);
			pages_tofree =
			shrink_pages(mm_scan, &zone0_page_list,
					&zone1_page_list, 0x7FFFFFFF);
			up_read(&mm_scan->mmap_sem);
			mmput(mm_scan);
			pages_freed =
			    swap_pages(&zone0_page_list,
				       &zone1_page_list);
			lmk_kill_ok = 0;

		}
	}

	return size;
}

static DEVICE_ATTR(lmk_state, 0664, lmk_state_show, lmk_state_store);

#endif /* CONFIG_ZRAM_FOR_ANDROID */

static int __init lowmem_init(void)
{
#ifdef CONFIG_ZRAM_FOR_ANDROID
	struct zone *zone;
	unsigned int high_wmark = 0;
	unsigned int low_wmark = 0;
#endif
	register_shrinker(&lowmem_shrinker);
#ifdef CONFIG_MEMORY_HOTPLUG
	hotplug_memory_notifier(lmk_hotplug_callback, 0);
#endif

#ifdef CONFIG_ZRAM_FOR_ANDROID
	for_each_zone(zone) {
		if (high_wmark < zone->watermark[WMARK_HIGH]) {
			high_wmark = zone->watermark[WMARK_HIGH];
			low_wmark = zone->watermark[WMARK_LOW];
		}
	}
	high_wmark += low_wmark;
	check_free_memory = (high_wmark != 0) ? high_wmark : CHECK_FREE_MEMORY;

	lmk_class = class_create(THIS_MODULE, "lmk");
	if (IS_ERR(lmk_class)) {
		printk(KERN_ERR "Failed to create class(lmk)\n");
		return 0;
	}
	lmk_dev = device_create(lmk_class, NULL, 0, NULL, "lowmemorykiller");
	if (IS_ERR(lmk_dev)) {
		printk(KERN_ERR
		       "Failed to create device(lowmemorykiller)!= %ld\n",
		       IS_ERR(lmk_dev));
		return 0;
	}
	if (device_create_file(lmk_dev, &dev_attr_lmk_state) < 0)
		printk(KERN_ERR "Failed to create device file(%s)!\n",
		       dev_attr_lmk_state.attr.name);
#endif /* CONFIG_ZRAM_FOR_ANDROID */

	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static int lowmem_oom_adj_to_oom_score_adj(int oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	int oom_adj;
	int oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_int,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
		    &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		    -1, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(adj, "array of int");
#else
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

