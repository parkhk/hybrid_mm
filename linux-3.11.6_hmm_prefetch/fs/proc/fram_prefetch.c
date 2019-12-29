#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmzone.h>
#include <linux/proc_fs.h>
#include <linux/quicklist.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/atomic.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <linux/fram.h>
#include "internal.h"

extern unsigned long check_fram_call[8];
extern unsigned long fram_ondemand_cnt;
extern unsigned long fram_prefetch_cnt;
extern unsigned long fram_hit_cnt;
extern unsigned long fram_prefetch_num;
extern unsigned int fram_prefetch_trigger;
static int fram_prefetch_proc_show(struct seq_file *m, void *v)
{
	init_fram_io_d();
	seq_printf(m,
		"Prefetch Trg   : %16lu\n"
		"Prefetch Number: %16lu\n"
		"fram ondemand  : %16lu\n"
		"fram prefetch  : %16lu\n"
		"fram hit       : %16lu\n"
		,
		fram_prefetch_trigger,
		fram_prefetch_num,
		fram_ondemand_cnt,
		fram_prefetch_cnt,
		fram_hit_cnt
			);

	return 0;
}

static ssize_t write_fram_prefetch(struct file *filp,const char *buf,size_t count,loff_t *offp)
{
	unsigned int i=0;
	fram_prefetch_num = simple_strtoul(&buf[0],NULL,0);
	while(buf[i++] != ' ');
	fram_prefetch_trigger = simple_strtoul(&buf[i],NULL,0);
	fram_ondemand_cnt=0;
	fram_prefetch_cnt=0;
	fram_hit_cnt=0;

	init_fram_io_d();
	printk(
		"Prefetch Trg   : %16lu\n"
		"Prefetch Number: %16lu\n"
		"fram ondemand  : %16lu\n"
		"fram prefetch  : %16lu\n"
		"fram hit       : %16lu\n"
		,
		fram_prefetch_trigger,
		fram_prefetch_num,
		fram_ondemand_cnt,
		fram_prefetch_cnt,
		fram_hit_cnt
			);

	return count;
}


static int fram_prefetch_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fram_prefetch_proc_show, NULL);
}

static const struct file_operations fram_prefetch_proc_fops = {
	.open		= fram_prefetch_proc_open,
	.read		= seq_read,
	.write		= write_fram_prefetch,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_fram_prefetch_init(void)
{
	proc_create("fram_prefetch", 0, NULL, &fram_prefetch_proc_fops);
	return 0;
}
module_init(proc_fram_prefetch_init);
