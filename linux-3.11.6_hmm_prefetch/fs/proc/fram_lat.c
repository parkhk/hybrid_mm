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
#include "internal.h"

extern unsigned long fram_read_lat;
extern unsigned long fram_write_lat;
static int fram_lat_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m,
		"how to set : echo 00 00 > /proc/fram_lat\n"
		"             echo 00 10 > /proc/fram_lat\n"
		"Fram Read  Latency : %8luus\n"
		"Fram Wrtie Latency : %8luus\n"
		,
		fram_read_lat,
		fram_write_lat
			);

	return 0;
}

static ssize_t write_fram_lat(struct file *filp,const char *buf,size_t count,loff_t *offp)
{
	unsigned int i=0;
	printk("in %s\n", __func__);
	fram_read_lat = simple_strtoul(&buf[0],NULL,0);
	while(buf[i++] != ' ');
	fram_write_lat = simple_strtoul(&buf[i],NULL,0);
	printk(
		"Fram Read  Latency : %8luus\n"
		"Fram Wrtie Latency : %8luus\n"
		,
		fram_read_lat,
		fram_write_lat
		);

	return count;
}


static int fram_lat_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fram_lat_proc_show, NULL);
}

static const struct file_operations fram_lat_proc_fops = {
	.open		= fram_lat_proc_open,
	.read		= seq_read,
	.write		= write_fram_lat,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_fram_lat_init(void)
{
	proc_create("fram_lat", 0, NULL, &fram_lat_proc_fops);
	return 0;
}
module_init(proc_fram_lat_init);
