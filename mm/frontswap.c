/*
 * Frontswap frontend
 *
 * This code provides the generic "frontend" layer to call a matching
 * "backend" driver implementation of frontswap.  See
 * Documentation/vm/frontswap.txt for more information.
 *
 * Copyright (C) 2009-2012 Oracle Corp.  All rights reserved.
 * Author: Dan Magenheimer
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/frontswap.h>
#include <linux/swapfile.h>

/*
 * frontswap_ops is set by frontswap_register_ops to contain the pointers
 * to the frontswap "backend" implementation functions.
 */
static struct frontswap_ops frontswap_ops __read_mostly;

/*
 * This global enablement flag reduces overhead on systems where frontswap_ops
 * has not been registered, so is preferred to the slower alternative: a
 * function call that checks a non-global.
 */
bool frontswap_enabled __read_mostly;
EXPORT_SYMBOL(frontswap_enabled);

/*
 * If enabled, frontswap_put will return failure even on success.  As
 * a result, the swap subsystem will always write the page to swap, in
 * effect converting frontswap into a writethrough cache.  In this mode,
 * there is no direct reduction in swap writes, but a frontswap backend
 * can unilaterally "reclaim" any pages in use with no data loss, thus
 * providing increases control over maximum memory usage due to frontswap.
 */
static bool frontswap_writethrough_enabled __read_mostly;

#ifdef CONFIG_DEBUG_FS
/*
 * Counters available via /sys/kernel/debug/frontswap (if debugfs is
 * properly configured).  These are for information only so are not protected
 * against increment races.
 */
static u64 frontswap_gets;
static u64 frontswap_succ_puts;
static u64 frontswap_failed_puts;
static u64 frontswap_invalidates;

static inline void inc_frontswap_gets(void) {
	frontswap_gets++;
}
static inline void inc_frontswap_succ_puts(void) {
	frontswap_succ_puts++;
}
static inline void inc_frontswap_failed_puts(void) {
	frontswap_failed_puts++;
}
static inline void inc_frontswap_invalidates(void) {
	frontswap_invalidates++;
}
#else
static inline void inc_frontswap_gets(void) { }
static inline void inc_frontswap_succ_puts(void) { }
static inline void inc_frontswap_failed_puts(void) { }
static inline void inc_frontswap_invalidates(void) { }
#endif
/*
 * Register operations for frontswap, returning previous thus allowing
 * detection of multiple backends and possible nesting.
 */
struct frontswap_ops frontswap_register_ops(struct frontswap_ops *ops)
{
	struct frontswap_ops old = frontswap_ops;

	frontswap_ops = *ops;
	frontswap_enabled = true;
	return old;
}
EXPORT_SYMBOL(frontswap_register_ops);

/*
 * Enable/disable frontswap writethrough (see above).
 */
void frontswap_writethrough(bool enable)
{
	frontswap_writethrough_enabled = enable;
}
EXPORT_SYMBOL(frontswap_writethrough);

/*
 * Called when a swap device is swapon'd.
 */
void __frontswap_init(unsigned type)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (sis->frontswap_map == NULL)
		return;
	frontswap_ops.init(type);
}
EXPORT_SYMBOL(__frontswap_init);

static inline void __frontswap_clear(struct swap_info_struct *sis, pgoff_t offset)
{
	frontswap_clear(sis, offset);
	atomic_dec(&sis->frontswap_pages);
}

/*
 * "Put" data from a page to frontswap and associate it with the page's
 * swaptype and offset.  Page must be locked and in the swap cache.
 * If frontswap already contains a page with matching swaptype and
 * offset, the frontswap implmentation may either overwrite the data and
 * return success or invalidate the page from frontswap and return failure.
 */
int __frontswap_put_page(struct page *page)
{
	int ret = -1, dup = 0;
	swp_entry_t entry = { .val = page_private(page), };
	int type = swp_type(entry);
	struct swap_info_struct *sis = swap_info[type];
	pgoff_t offset = swp_offset(entry);

	BUG_ON(!PageLocked(page));
	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset))
		dup = 1;
	ret = (*frontswap_ops.put_page)(type, offset, page);
	if (ret == 0) {
		frontswap_set(sis, offset);
		inc_frontswap_succ_puts();
		if (!dup)
			atomic_inc(&sis->frontswap_pages);
	} else {
		/*
		  failed dup always results in automatic invalidate of
		  the (older) page from frontswap
		 */
		if (dup)
			__frontswap_clear(sis, offset);
	}
		if (frontswap_writethrough_enabled)
		/* report failure so swap also writes to swap device */
		ret = -1;
	return ret;
}
EXPORT_SYMBOL(__frontswap_put_page);

/*
 * "Get" data from frontswap associated with swaptype and offset that were
 * specified when the data was put to frontswap and use it to fill the
 * specified page with data. Page must be locked and in the swap cache.
 */
int __frontswap_get_page(struct page *page)
{
	int ret = -1;
	swp_entry_t entry = { .val = page_private(page), };
	int type = swp_type(entry);
	struct swap_info_struct *sis = swap_info[type];
	pgoff_t offset = swp_offset(entry);

	BUG_ON(!PageLocked(page));
	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset))
		ret = (*frontswap_ops.get_page)(type, offset, page);
	if (ret == 0)
		inc_frontswap_gets();
	return ret;
}
EXPORT_SYMBOL(__frontswap_get_page);

/*
 * Invalidate any data from frontswap associated with the specified swaptype
 * and offset so that a subsequent "get" will fail.
 */
void __frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (frontswap_test(sis, offset)) {
		(*frontswap_ops.invalidate_page)(type, offset);
		atomic_dec(&sis->frontswap_pages);
		frontswap_clear(sis, offset);
		inc_frontswap_invalidates();
	}
}
EXPORT_SYMBOL(__frontswap_invalidate_page);

/*
 * Invalidate all data from frontswap associated with all offsets for the
 * specified swaptype.
 */
void __frontswap_invalidate_area(unsigned type)
{
	struct swap_info_struct *sis = swap_info[type];

	BUG_ON(sis == NULL);
	if (sis->frontswap_map == NULL)
		return;
	(*frontswap_ops.invalidate_area)(type);
	atomic_set(&sis->frontswap_pages, 0);
	bitmap_zero(sis->frontswap_map, sis->max);
}
EXPORT_SYMBOL(__frontswap_invalidate_area);

static unsigned long __frontswap_curr_pages(void)
{
	int type;
	unsigned long totalpages = 0;
	struct swap_info_struct *si = NULL;

	assert_spin_locked(&swap_lock);
	for (type = swap_list.head; type >= 0; type = si->next) {
		si = swap_info[type];
		totalpages += atomic_read(&si->frontswap_pages);
	}
	return totalpages;
}

static int __frontswap_unuse_pages(unsigned long total, unsigned long *unused,
					int *swapid)
{
	int ret = -EINVAL;
	struct swap_info_struct *si = NULL;
	int si_frontswap_pages;
	unsigned long total_pages_to_unuse = total;
	unsigned long pages = 0, pages_to_unuse = 0;
	int type;

	assert_spin_locked(&swap_lock);
	for (type = swap_list.head; type >= 0; type = si->next) {
		si = swap_info[type];
		si_frontswap_pages = atomic_read(&si->frontswap_pages);
		if (total_pages_to_unuse < si_frontswap_pages) {
			pages = pages_to_unuse = total_pages_to_unuse;
		} else {
			pages = si_frontswap_pages;
			pages_to_unuse = 0; /* unuse all */
		}
		/* ensure there is enough RAM to fetch pages from frontswap */
		if (security_vm_enough_memory_mm(current->mm, pages)) {
			ret = -ENOMEM;
			continue;
		}
		vm_unacct_memory(pages);
		*unused = pages_to_unuse;
		*swapid = type;
		ret = 0;
		break;
	}

	return ret;
}

/*
 * Used to check if it's necessory and feasible to unuse pages.
 * Return 1 when nothing to do, 0 when need to shink pages,
 * error code when there is an error.
 */
static int __frontswap_shrink(unsigned long target_pages,
				unsigned long *pages_to_unuse,
				int *type)
{
	unsigned long total_pages = 0, total_pages_to_unuse;

	assert_spin_locked(&swap_lock);

	total_pages = __frontswap_curr_pages();
	if (total_pages <= target_pages) {
		/* Nothing to do */
		*pages_to_unuse = 0;
		return 1;
	}
	total_pages_to_unuse = total_pages - target_pages;
	return __frontswap_unuse_pages(total_pages_to_unuse, pages_to_unuse, type);
}

/*
 * Frontswap, like a true swap device, may unnecessarily retain pages
 * under certain circumstances; "shrink" frontswap is essentially a
 * "partial swapoff" and works by calling try_to_unuse to attempt to
 * unuse enough frontswap pages to attempt to -- subject to memory
 * constraints -- reduce the number of pages in frontswap to the
 * number given in the parameter target_pages.
 */
void frontswap_shrink(unsigned long target_pages)
{
	unsigned long pages_to_unuse = 0;
	int uninitialized_var(type), ret;

	/*
	 * we don't want to hold swap_lock while doing a very
	 * lengthy try_to_unuse, but swap_list may change
	 * so restart scan from swap_list.head each time
	 */
	spin_lock(&swap_lock);
	ret = __frontswap_shrink(target_pages, &pages_to_unuse, &type);
	spin_unlock(&swap_lock);
	if (ret == 0)
		try_to_unuse(type, true, pages_to_unuse);
	return;
}
EXPORT_SYMBOL(frontswap_shrink);

/*
 * Count and return the number of frontswap pages across all
 * swap devices.  This is exported so that backend drivers can
 * determine current usage without reading debugfs.
 */
unsigned long frontswap_curr_pages(void)
{
	unsigned long totalpages = 0;

	spin_lock(&swap_lock);
	totalpages = __frontswap_curr_pages();
	spin_unlock(&swap_lock);

	return totalpages;
}
EXPORT_SYMBOL(frontswap_curr_pages);

static int __init init_frontswap(void)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *root = debugfs_create_dir("frontswap", NULL);
	if (root == NULL)
		return -ENXIO;
	debugfs_create_u64("gets", S_IRUGO, root, &frontswap_gets);
	debugfs_create_u64("succ_puts", S_IRUGO, root, &frontswap_succ_puts);
	debugfs_create_u64("failed_puts", S_IRUGO, root,
				&frontswap_failed_puts);
	debugfs_create_u64("invalidates", S_IRUGO,
				root, &frontswap_invalidates);
#endif
	return 0;
}

module_init(init_frontswap);
