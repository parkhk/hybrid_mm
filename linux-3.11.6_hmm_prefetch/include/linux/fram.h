#include <linux/mmzone.h>
#include <linux/rmap.h>
#include <linux/page-flags.h>

#include <asm/delay.h>

#define SHADOW_OFFSET 512

extern struct fram_flash_pages fram_flash_pages;
//extern void free_flash_page(struct page *page);
//extern struct page *alloc_flash_page(void);
//read
extern int fram_pages_in(struct page *dram_page, struct page *flash_page, int flag);
//write
extern int fram_pages_out(struct page *dram_page, struct page *flash_page, int flag);
extern inline struct page *fram_reserve_pages(void);
extern inline unsigned long fram_release_pages(struct page *page);

extern int add_to_evict(struct page *);

extern struct page * fetch_page_cache(struct page *flash_page, struct address_space *mapping);
extern int evict_page_cache(struct page *old, struct page *new, unsigned int fetch);
extern void flash_page_add_rmap(struct page *page, struct vm_area_struct *vma, unsigned long address);
extern void flash_page_remove_rmap(struct page *);

extern int try_to_unmap_victim(struct page *, enum ttu_flags flags);
extern int try_to_map_fetched_page(struct page *);

extern int try_to_free_flash(struct page *);
extern void delete_fram_page_cache(struct page *flash_page);
extern void init_fram_io_d(void);
