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

#define NVME_SECTOR_SHIFT (3)
#define PAGE2LBA(x)	( page_to_pfn(x) << NVME_SECTOR_SHIFT )
#define LBA2PAGE(x)	( pfn_to_page((x >> NVME_SECTOR_SHIFT)))
#define BATCH_COUNT	16


struct fram_flash_pages fram_flash_pages;
EXPORT_SYMBOL(fram_flash_pages);

struct block_device *fram_blk_dev;
EXPORT_SYMBOL(fram_blk_dev);

int fram_ready = 0;
EXPORT_SYMBOL(fram_ready);


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
				(page->flags & PAGE_FLAGS_CHECK_AT_FREE))) { //TODO TODO shengqiu, BUG page state
		printk(KERN_EMERG "free_flash_check: BUG: mapcount : %d, count : %d page->flags : %lx Bad nand page state in process %s  pfn:%05lx in free\n",
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
		printk(KERN_EMERG "prep_new_flash_page: BUG: mapcount : %d, count : %d  page->flags : %lx Bad nand page state in process %s  pfn:%05lx in alloc\n",
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
	list_add(&page->lru, &fram_pcp->free_lists);
	fram_pcp->free_count++;
	if (fram_pcp->free_count >= fram_pcp->high) {
		unsigned int cnt = fram_pcp->batch;
		spin_lock(&fram_flash_pages.lock);
		while(cnt--){
			page = list_entry(fram_pcp->free_lists.next, struct page, lru);
			list_move(&page->lru, &fram_flash_pages.list);
		}
		fram_flash_pages.count += fram_pcp->batch;
		spin_unlock(&fram_flash_pages.lock);
		fram_pcp->free_count -= fram_pcp->batch;
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
	list = &fram_pcp->alloc_lists;
	if (list_empty(list)) {
		unsigned int cnt;
		spin_lock(&fram_flash_pages.lock);
		for(cnt = 0 ; cnt < fram_pcp->batch && fram_flash_pages.count ; cnt++){
			page = list_entry(fram_flash_pages.list.prev, struct page, lru);
			list_move(&page->lru, list);
			fram_flash_pages.count--;
		}
		spin_unlock(&fram_flash_pages.lock);
		fram_pcp->alloc_count += cnt;
		if (unlikely(list_empty(list))){
			printk(KERN_EMERG"[LOG - Fail get fram_page from global list[%d:%s]\n", __LINE__, __func__);
			goto failed;
		}
	}

	page = list_entry(list->next, struct page, lru);
	list_del(&page->lru);
	fram_pcp->alloc_count--;
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
/*shengqiu add, stats for page in-out between fram and dram*/
unsigned long fram_page_in_nums;
unsigned long fram_page_out_nums;
EXPORT_SYMBOL(fram_page_in_nums);
EXPORT_SYMBOL(fram_page_out_nums);
EXPORT_SYMBOL(fram_read_lat);
EXPORT_SYMBOL(fram_write_lat);

/*struct fram_ops {
  u32 (*get_page) ( struct page *dram_page, struct page *flash_page );
  u32 (*put_page)  ( struct page *dram_page, struct page *flash_page, int flag );

};

struct fram_ops *fram_ops = 0;

u32 register_fram_ops(struct fram_ops *ops)
{
  	fram_ops = ops;

  	printk("fram ops registered %p\n", ops);
  	return 0;
}
EXPORT_SYMBOL(register_fram_ops);
u32 unregister_fram_ops(struct fram_ops *ops)

{
  if (fram_ops != ops)
    printk("fram ops was not the same as the being unregistered one\n");
  fram_ops = 0;
  printk("fram ops unregistered %p\n", ops);
  return 0;
}
EXPORT_SYMBOL(unregister_fram_ops);*/


static void end_fram_bio_read(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;

	if(!uptodate){
		SetPageError(page);
		ClearPageUptodate(page);
		printk(KERN_ERR "read-error on flash lba %Lu\n", 
				(unsigned long long)bio->bi_sector);
		goto out;
	}
	
	SetPageUptodate(page);
out:
	unlock_page(page);
	bio_put(bio);
}

static void end_fram_bio_write(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;
	struct page *flash_page = pfn_to_page(page_private(page));	

	//VM_BUG_ON(!PageFramIO(page));

	if(!uptodate){
		SetPageError(page);

		set_page_dirty(page);
		printk(KERN_ERR "write-error on flash lba %Lu\n", 
					(unsigned long long)bio->bi_sector);
		ClearPageReclaim(page);
	}
	
	TestClearPageFramwriteback(page);
	//ClearPageFramIO(page);		
	//unlock_page(flash_page);
	//unlock_page(page);
	bio_put(bio);	
}

static void end_fram_pagecache_bio_read(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
        struct page *page = bio->bi_io_vec[0].bv_page;
	unsigned long flash_pfn = bio->bi_sector >> NVME_SECTOR_SHIFT;
	struct page *flash_page = pfn_to_page(flash_pfn);

	VM_BUG_ON(!flash_page);
	VM_BUG_ON(!PageFlash(flash_page));
	VM_BUG_ON(!PageLocked(page));

	if(!uptodate){
                SetPageError(page);
                ClearPageUptodate(page);
                printk(KERN_ERR "read-error on flash lba %Lu\n",
                                (unsigned long long)bio->bi_sector);
                goto out;
        }

        SetPageUptodate(page);
//	printk(KERN_ERR "end_fram_pagecache_bio_read: finish sector=%lu\n", bio->bi_sector);
out:
	atomic_set(&flash_page->_count, 0);	
	set_page_private(flash_page, 0);
//	flash_page->flags &= ~FLASH_PAGE_FLAGS_CHECK_AT_PREP;
	free_hot_cold_page(flash_page, 1);
	unlock_page(page);
	bio_put(bio);
}

static void end_fram_pagecache_bio_write(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct page *page = bio->bi_io_vec[0].bv_page;
	struct page *flash_page = pfn_to_page(page_private(page));

//	VM_BUG_ON(!PageFramIO(page));
	VM_BUG_ON(!page_private(page));
	VM_BUG_ON(!flash_page);
	VM_BUG_ON(!PageFlash(flash_page));

	if(!uptodate){
//		SetPageError(page);
		
//		set_page_dirty(page);
                printk(KERN_ERR "write-error on flash lba %Lu\n",
                                        (unsigned long long)bio->bi_sector);
//		ClearPageReclaim(page);
		goto out;
        }

//	printk(KERN_ERR "end_fram_pagecache_bio_write: finish sector=%lu\n", bio->bi_sector);

out:
	//ClearPageFramIO(page);
//	unlock_page(page);
	unlock_page(flash_page);
//	page_cache_release(flash_page);
	bio_put(bio);
}

/*compose a bio for ANON page out*/
static struct bio *get_fram_bio(gfp_t gfp_flags, 
			struct page *dram_page, struct page *flash_page, 
			bio_end_io_t end_io)
{//TODO
	struct bio *bio;
	
	if(!fram_blk_dev)
		printk(KERN_ERR "get_fram_bio: empty block_device \n");

	bio = bio_alloc(gfp_flags, 1);
	if(bio){
		bio->bi_sector = PAGE2LBA(flash_page);
		bio->bi_bdev = fram_blk_dev;
		bio->bi_io_vec[0].bv_page = dram_page;
		bio->bi_io_vec[0].bv_len = PAGE_SIZE;
		bio->bi_io_vec[0].bv_offset = 0;
		bio->bi_vcnt = 1;
		bio->bi_size = PAGE_SIZE;
		bio->bi_end_io = end_io;
	}
	return bio;
}

/*do asynchronous IO read for anon page*/
static int fram_read(struct page *dram_page, struct page *flash_page, int flag)
{//TODO
	struct bio *bio;
	int ret = 0;
	
	VM_BUG_ON(!PageLocked(dram_page));
	VM_BUG_ON(PageUptodate(dram_page));

	if(!flag)
		bio = get_fram_bio(GFP_KERNEL, dram_page, flash_page, end_fram_bio_read);
	else
		bio = get_fram_bio(GFP_KERNEL, dram_page, flash_page, end_fram_pagecache_bio_read);
	if(bio == NULL){
		printk(KERN_ERR "fram_read: fail to alloc bio for read\n");
		unlock_page(dram_page);
		ret = -ENOMEM;
		goto out;
	}
	submit_bio(READ, bio);
out:
	return ret;	
}

/*do asynchronous IO write for anon page*/
static int fram_write(struct page *dram_page, struct page *flash_page, int flag)
{//TODO
	struct bio *bio;
	int ret = 0, rw = WRITE;

	if(flag == 0) /*ANON page out*/
		bio = get_fram_bio(GFP_NOIO, dram_page, flash_page, end_fram_bio_write); 
	else /*file page cache out*/
		bio = get_fram_bio(GFP_NOIO, dram_page, flash_page, 
							end_fram_pagecache_bio_write);
	if(bio == NULL){
		set_page_dirty(dram_page);
		unlock_page(dram_page);
		ret = -ENOMEM;
		goto out;
	}

	TestSetPageFramwriteback(dram_page);
	ClearPageDirty(dram_page);
	unlock_page(dram_page);
	submit_bio(rw, bio);

out:
	return ret;
}

int fram_pages_in(struct page *dram_page, struct page *flash_page, int flag){
/*  if(!fram_ready)
  	fram_blk_dev = blkdev_get_by_path("/dev/sdd",
                                        FMODE_READ|FMODE_WRITE, NULL);
  if(fram_blk_dev)
	fram_ready = 1;
*/	
   __set_page_locked(dram_page);
 // if (fram_ready) {
    fram_page_in_nums += 1;
    return fram_read(dram_page, flash_page, flag);
  /*} else {	
	udelay(fram_read_lat);
	memcpy(page_address(dram_page), page_address(flash_page), PAGE_SIZE);
	return  0;
  }*/
}

//write
int fram_pages_out(struct page *dram_page, struct page *flash_page, int flag){
  /*if(!fram_ready)
        fram_blk_dev = blkdev_get_by_path("/dev/sdd",
                                        FMODE_READ|FMODE_WRITE, NULL);
  if(fram_blk_dev)
        fram_ready = 1;	

  if (fram_ready) {*/
    fram_page_out_nums += 1;
    return fram_write(dram_page, flash_page, flag);
  /*} else {
	udelay(fram_write_lat);
	memcpy(page_address(flash_page), page_address(dram_page), PAGE_SIZE);
	return  0;
  }*/
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

void init_fram_io_d(void)
{
  if(!fram_ready)
        fram_blk_dev = blkdev_get_by_path("/dev/sdd",
                                        FMODE_READ|FMODE_WRITE, NULL);
  if(fram_blk_dev){
        fram_ready = 1;	
		printk("parkhk : fram ssd is enabled\n");
  }else
		printk("parkhk : fram ssd is disabled\n");
}
EXPORT_SYMBOL(init_fram_io_d);
