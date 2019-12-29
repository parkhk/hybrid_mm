/*
 *  linux/mm/hybrid_mm/nand_alloc.c
 *
 *  Manages the nand free pages, the system allocates nand pages here.
 *  The allocator only manages single page frame. 
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mempolicy.h>
#include <linux/stop_machine.h>
#include <linux/sort.h>
#include <linux/pfn.h>
#include <linux/backing-dev.h>
#include <linux/fault-inject.h>
#include <linux/page-isolation.h>
#include <linux/page-flags.h>
#include <linux/page_cgroup.h>
#include <linux/debugobjects.h>
#include <linux/kmemleak.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <linux/ftrace_event.h>
#include <linux/memcontrol.h>
#include <linux/prefetch.h>
#include <linux/migrate.h>
#include <linux/page-debug-flags.h>
#include <linux/mmdebug.h>
#include <linux/page-flags.h>
#include <linux/fram.h>

#include <asm/tlbflush.h>
#include <asm/div64.h>

struct fram_flash_pages fram_flash_pages;
EXPORT_SYMBOL(fram_flash_pages);

static inline int flash_page_mapcount(struct page *page)
{
	return atomic_read(&(page)->_mapcount) + 1;
}

static bool free_flash_check(struct page *page)
{
	trace_mm_flash_free(page);

	//Anonymous page
	//if (((unsigned long)page->mapping & PAGE_MAPPING_ANON) != 0)
		page->mapping = NULL;

	if (unlikely(flash_page_mapcount(page) |
				(page->mapping != NULL)  |
				(atomic_read(&page->_count) != 0) |
				(page->flags & PAGE_FLAGS_CHECK_AT_FREE))) {
		printk(KERN_EMERG "BUG: mapcount : %d, count : %d page->flags : %lx Bad nand page state in process %s  pfn:%05lx in free\n",
				flash_page_mapcount(page), atomic_read(&page->_count), page->flags, current->comm, page_to_pfn(page));
		return false;
	}
	if (page->flags & FLASH_PAGE_FLAGS_CHECK_AT_PREP)
		page->flags &= ~FLASH_PAGE_FLAGS_CHECK_AT_PREP;
	return true;
}

static inline void set_flash_page_refcounted(struct page *page)
{
	VM_BUG_ON(PageTail(page));
	VM_BUG_ON(atomic_read(&page->_count));
	atomic_set(&page->_count, 1);
}

static int prep_new_flash_page(struct page *page)
{
	if (unlikely(flash_page_mapcount(page) |
				(page->mapping != NULL)  |
				(atomic_read(&page->_count) != 0)  |
				(page->flags & FLASH_PAGE_FLAGS_CHECK_AT_PREP))) {
		printk(KERN_EMERG "BUG: mapcount : %d, count : %d  page->flags : %lx Bad nand page state in process %s  pfn:%05lx in alloc\n",
				flash_page_mapcount(page), atomic_read(&page->_count), page->flags, current->comm, page_to_pfn(page));
		return 1;
	}	

	set_page_private(page, 0);
	set_flash_page_refcounted(page);

	return 0;
}

void free_flash_page(struct page *page)
{
	struct fram_per_cpu_pages *fram_pcp;
	unsigned long flags;

	if (!free_flash_check(page))
		return;

	local_irq_save(flags);

	fram_pcp = this_cpu_ptr(fram_flash_pages.fram_pcp);
	list_add(&page->lru, &fram_pcp->lists);
	fram_pcp->count++;
	if (fram_pcp->count >= fram_pcp->high) {
		unsigned int cnt = fram_pcp->batch;
		spin_lock(&fram_flash_pages.lock);
		while(cnt--){
			page = list_entry(fram_pcp->lists.next, struct page, lru);
			list_move(&page->lru, &fram_flash_pages.list);
		}
		fram_flash_pages.count += fram_pcp->batch;
		spin_unlock(&fram_flash_pages.lock);
		fram_pcp->count -= fram_pcp->batch;
	}

	local_irq_restore(flags);
}
//EXPORT_SYMBOL(free_flash_page);

struct page *alloc_flash_page(void)
{
	unsigned long flags;
	struct page *page;
	struct fram_per_cpu_pages *fram_pcp;
	struct list_head *list;

again:
	local_irq_save(flags);
	fram_pcp = this_cpu_ptr(fram_flash_pages.fram_pcp);
	list = &fram_pcp->lists;
	if (list_empty(list)) {
		unsigned int cnt;
		spin_lock(&fram_flash_pages.lock);
		for(cnt = 0 ; cnt < fram_pcp->batch && fram_flash_pages.count ; cnt++){
			page = list_entry(fram_flash_pages.list.next, struct page, lru);
			list_move(&page->lru, list);
			fram_flash_pages.count--;
		}
		spin_unlock(&fram_flash_pages.lock);
		fram_pcp->count += cnt;
		if (unlikely(list_empty(list))){
			printk(KERN_EMERG"[LOG - Fail get fram_page from global list[%d:%s]\n", __LINE__, __func__);
			goto failed;
		}
	}

	page = list_entry(list->next, struct page, lru);
	list_del(&page->lru);
	fram_pcp->count--;
	local_irq_restore(flags);
	if(prep_new_flash_page(page))
		goto again;
	return page;

failed:
	local_irq_restore(flags);
	return NULL;
}
//EXPORT_SYMBOL(alloc_flash_page);
//read
unsigned long fram_read_lat;
unsigned long fram_write_lat;
EXPORT_SYMBOL(fram_read_lat);
EXPORT_SYMBOL(fram_write_lat);
inline unsigned long fram_pages_in(struct page *dram_page, struct page *flash_page){
	//A page must held a lock_page for IO
	__set_page_locked(dram_page);
	udelay(fram_read_lat);
	memcpy(page_address(dram_page), page_address(flash_page), PAGE_SIZE);
	
	unlock_page(dram_page);
	return  0;
}

//write
inline unsigned long fram_pages_out(struct page *dram_page, struct page *flash_page){
	BUG_ON(!PageLocked(dram_page));
	//A page must not be under page lock for IO
	unlock_page(dram_page);
	TestSetPageWriteback(dram_page);
	udelay(fram_write_lat);
	memcpy(page_address(flash_page), page_address(dram_page), PAGE_SIZE);
	/*
	 * If the page is written to flash memory, the dirty bit & writeback bit should be cleared using follow macro
	 *  - TestClearPageWriteback(page)
	 * 	- ClearPageDirty(page)
	 */
	TestClearPageWriteback(dram_page);
	ClearPageDirty(dram_page);
	return  0;
}

inline struct page *fram_reserve_pages(void){
		return alloc_flash_page(); 
}

inline unsigned long fram_release_pages(struct page *page){
	if(TestClearPageFrampc(page)){
		unsigned long flags;
		spin_lock_irqsave(&fram_flash_pages.fram_pagecache.lock, flags);
#ifdef CONFIG_SLAB
		list_del(&page->fram_lru);
#endif
		fram_flash_pages.fram_pagecache.count--;
		spin_unlock_irqrestore(&fram_flash_pages.fram_pagecache.lock, flags);
	}
	free_flash_page(page);
	return  0;
}
