/* tvbox_i8xx.c
 *
 * kernel-level portion of Tv Box v3.0 rendering engine.
 * handles the icky details of allocating a block of physical RAM
 * that's unbroken and uncached, so that userspace can worry about
 * the rest.
 *
 * THIS IS NOT OPEN SOURCE.
 *
 * We do not concern ourself with the possibility of multiple Intel
 * graphics chipsets, because the graphics device is never really
 * an independent device, it's always an embedded part of the
 * PCI northbridge, and usually at a fixed PCI bus/dev/fn, and
 * there is never more than one. Maybe someday if Intel starts
 * doing weird things like two devices...
 *
 * (C) 2009 Impact Studio Pro.
 */

#include <linux/miscdevice.h>
#include <linux/capability.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/parser.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mman.h>
#include <linux/pci.h>
#include <linux/vfs.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <asm/io.h>

#include "tvbox_i8xx.h"

#if defined(DEBUG_ME)
# define DBG_(x,...) printk(KERN_INFO "tvbox_i8xx: " x "\n", __VA_ARGS__ )
# define DBG(x) printk(KERN_INFO "tvbox_i8xx: " x "\n")
#else
# define DBG_(x,...) { }
# define DBG(x) { }
#endif

static unsigned long MB(unsigned long x) { return x << 20UL; }

/* on behalf of the user-space application we grab a 512KB region and hold onto it.
 * Intel's page table design allows the framebuffer and AGP aperature to literally happen anywhere
 * in system RAM even if far-flung across fragmented pages, BUT the page table itself must exist
 * in an unbroken section of RAM that the CPU treats as uncacheable. So, if we do that part, user-space
 * can worry about the rest.
 *
 * the reason the code below us saves the sum of the aperatures is that on systems with a two-function
 * display controller the userspace daemon might want both to function at the same time, with separate
 * page tables for each. the page table has to be large enough have one DWORD per page of aperature.
 * two aperatures: double the space. */
static uint32_t*	pgtable = NULL;
static unsigned long	pgtable_base = 0;
static size_t		pgtable_base_phys = 0;
static size_t		pgtable_size = 0;
static unsigned int	pgtable_order = 0;
/* handy way for programmer reference. pgtable_size is in bytes */
#define pgtable_entries (pgtable_size / 4)

static void free_pgtable(void) {
	if (pgtable_base) {
		DBG("Freeing pagetable");
		free_pages(pgtable_base,pgtable_order);
		pgtable_base = (unsigned long)NULL;
		pgtable_base_phys = 0;
		pgtable_order = 0;
		pgtable_size = 0;
		pgtable = NULL;
	}
}

static int alloc_pgtable(void) {
	unsigned int pages = pgtable_size >> PAGE_SHIFT;
	if (pages == 0) return -ENOMEM;

	/* what "order" is the size? */
	for (pgtable_order=0;((pages - 1) >> pgtable_order) != 0;) {
		if (++pgtable_order >= 16) {	/* don't ask for anything too large */
			DBG_("cannot compute order for %u pages",pages);
			return -ENOMEM;
		}
	}

	DBG_("pagetable: page order %u for %u pages",pgtable_order,pages);

	pgtable_base = __get_free_pages(GFP_KERNEL,pgtable_order);
	if (pgtable_base == 0) {
		DBG("Allocation failed");
		return -ENOMEM;
	}

	DBG_("pagetable: Allocated at 0x%08lX size 0x%08lX",(unsigned long)pgtable_base,(unsigned long)pgtable_size);
	pgtable_base_phys = virt_to_phys((void*)pgtable_base);
	DBG_("pagetable: Physical memory location 0x%08X",pgtable_base_phys);
	pgtable = (uint32_t*)pgtable_base;

	/* make sure it's "size aligned", intel h/w demands it.
	 * fortunately, the Linux kernel seems to also return page orders that are size aligned */
	{
		unsigned long sz = 1UL << (PAGE_SHIFT + pgtable_order);
		if ((pgtable_base & (sz - 1)) != 0)
			DBG_("pagetable: Linux gave us non-size-aligned memory! 0x%08lX & 0x%08lX == 0x%08lX",
				pgtable_base,sz-1,pgtable_base&sz);
	}

	/* finally, the region needs to be uncacheable so that our updates (or userspace's) take effect immediately */
	if (set_memory_uc(pgtable_base,pages))
		DBG("Warning, unable to make pages uncacheable");

	return 0;
}

/* on every Intel graphics-based laptop I own, the BIOS takes 8MB off the top of RAM (just underneath
 * the SMM area) and declares that the framebuffer. The VESA BIOS on top of that takes the last 512KB
 * of the "framebuffer" and builds a page-table there, giving VESA BIOS clients 7.5MB of video memory
 * with the last page repeated to cloak the page table from the host system). to facilitate restoring
 * the framebuffer safely on unload or on command, we have to mimick that behavior, so we have to know
 * exactly how much RAM is really in the machine, and where the VESA BIOS stuck the framebuffer.
 *
 * it's vital we be able to do that, if we allow garbage to map the aperature there's no telling what
 * system memory would be corrupted when Linux fbcon writes to video memory. scary, huh?
 *
 * fixme: what will you do here when Intel chipsets have to deal with >= 4GB of RAM and stolen memory
 * in high memory? */
static size_t		intel_total_memory = 0;
static size_t		intel_stolen_base = 0;
static size_t		intel_stolen_size = 0;

/* Intel PCI device information. Sum of aperatures if more than one. */
static size_t		aperature_size = 0;
static size_t		aperature_base = 0;	/* first aperature only */
static int		chipset = 0;

/* only the first device's MMIO */
static size_t			mmio_base = 0;
static size_t			mmio_size = 0;
static volatile uint32_t*	mmio = NULL;

#define MMIO(x)			( *( mmio + ((x) >> 2) ) )

enum {
	/* sorry these are all the test subjects I have */
	CHIP_855,	/* 855GM chipsets */
	CHIP_965	/* 965 chipset */
};

static int map_mmio(void) {
	if (mmio_base == 0 || mmio_size == 0)
		return -ENODEV;

	if (mmio != NULL) /* no leaking! */
		return 0;

	mmio = (volatile uint32_t*)ioremap(mmio_base,mmio_size);
	if (mmio == NULL)
		return -ENODEV;

	DBG_("mmap mmio: 0x%08lX phys 0x%08lX",(unsigned long)mmio,(unsigned long)mmio_base);
	return 0;
}

static void unmap_mmio(void) {
	if (mmio != NULL) {
		DBG_("unmap mmio: 0x%08X",(size_t)mmio);
		iounmap((void*)mmio);
		mmio = NULL;
	}
}

static size_t find_intel_aperature(struct pci_dev *dev,size_t *c_base) {
	size_t base=0,size=0;
	int bar;

	for (bar=0;bar < PCI_ROM_RESOURCE;bar++) {
		struct resource *res = &dev->resource[bar];

		/* the aperature/framebuffer is the large one that is marked "prefetchable" */
		if ((res->flags & IORESOURCE_MEM) && (res->flags & IORESOURCE_PREFETCH) &&
			!(res->flags & IORESOURCE_DISABLED) && res->start != 0 && base == 0) {
			base = res->start;
			size = (res->end - res->start) + 1;
		}
	}

	if (c_base != NULL)
		*c_base = base;

	return size;
}

static size_t find_intel_mmio(struct pci_dev *dev,size_t *c_base) {
	size_t base=0,size=0;
	int bar;

	for (bar=0;bar < PCI_ROM_RESOURCE;bar++) {
		struct resource *res = &dev->resource[bar];

		/* the aperature/framebuffer is the small one that is marked "non-prefetchable" */
		if ((res->flags & IORESOURCE_MEM) && !(res->flags & IORESOURCE_PREFETCH) &&
			!(res->flags & IORESOURCE_DISABLED) && res->start != 0 && base == 0) {
			base = res->start;
			size = (res->end - res->start) + 1;
		}
	}

	if (c_base != NULL)
		*c_base = base;

	return size;
}

static int get_855_stolen_memory_info(struct pci_bus *bus) {
	uint16_t w;

	intel_stolen_base = 0;
	intel_stolen_size = 0;

	/* Host Hub Interface Bridge dev 0 */
	if (pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0x52,&w)) {
		DBG_("Whoah! Cannot read PCI configuration space word @ 0x%X",0x52);
		return -ENODEV;
	}

	DBG_("Intel 855 HHIB CFG word 0x52: 0x%04X",w);

	switch ((w >> 4) & 3) {
		case 1:	intel_stolen_size = MB(1);	break;
		case 2:	intel_stolen_size = MB(4);	break;
		case 3: intel_stolen_size = MB(8);	break;
		case 4:	intel_stolen_size = MB(16);	break;
		case 5:	intel_stolen_size = MB(32);	break;
	}

	/* take "total ram" estimate from Linux, round up to likely 32MB multiple,
	 * subtract 1MB for the SMM area, and subtract for the stolen base, and...
	 * that's where it starts */
	{
		struct sysinfo s;
		si_meminfo(&s);
		intel_stolen_base = s.totalram * s.mem_unit;
		DBG_("sysinfo: total ram pages %lu mem unit %lu",(unsigned long)s.totalram,(unsigned long)s.mem_unit);

		/* Linux get's it's total memory report from the BIOS, who of course
		 * returns total ram - 1MB - stolen RAM. so we have to round back up
		 * to what is most likely. Intel docs imply the chipset can only handle
		 * amounts of RAM up to the nearest 32MB or 64MB multiple */
		intel_stolen_base += MB(16) + intel_stolen_size;
		intel_stolen_base &= ~(MB(32) - 1);

		intel_total_memory = intel_stolen_base;

		intel_stolen_base -= MB(1);	/* System Management Mode area */
		intel_stolen_base -= intel_stolen_size;
	}

	DBG_("Stolen memory: %uMB @ 0x%08X",intel_stolen_size >> 20U,intel_stolen_base);
	if (intel_stolen_size == 0 || intel_stolen_base == 0)
		return -ENODEV;

	return 0;
}

static int get_855_info(struct pci_bus *bus,int slot) {
	struct pci_dev *primary = pci_get_slot(bus,PCI_DEVFN(slot,0));		/* primary function */
	struct pci_dev *secondary = pci_get_slot(bus,PCI_DEVFN(slot,1));	/* for those with secondary function for second head */

	if (primary == NULL)
		return -ENODEV;

	/* first, primary device */
	aperature_size = find_intel_aperature(primary,&aperature_base);
	if (aperature_size > 0) {
		DBG_("First aperature: @ 0x%08X size %08X",aperature_base,aperature_size);

		if (secondary) {
			/* secondary? */
			size_t second_size,second_base;
			second_size = find_intel_aperature(secondary,&second_base);
			if (second_size > 0) {
				DBG_("Second aperature: @ 0x%08X size %08X",second_base,second_size);
				aperature_size += second_size;
			}
		}
	}

	/* primary device: get MMIO */
	mmio_size = find_intel_mmio(primary,&mmio_base);
	if (mmio_base != 0 && mmio_size != 0)
		DBG_("First MMIO @ 0x%08X size %08X",mmio_base,mmio_size);

	if (aperature_size > 0)
		DBG_("Total aperature size: 0x%08X %uMB",aperature_size,aperature_size >> 20UL);

	if (get_855_stolen_memory_info(bus))
		return -ENODEV;

	return (aperature_base != 0 && aperature_size != 0) ? 0 : -ENODEV;
}

static int find_intel_graphics(void) {
	int slot,ret = -ENODEV;
	struct pci_bus *bus = pci_find_bus(0,0);	/* the important ones are always on the first bus e.g. 0:2:0 */
	if (!bus) {
		DBG("pci_find_bus(0,0) returned nothing");
		return ret;
	}

	DBG("found first PCI bus");

	/* Intel graphics chipsets are always #2 or #3 or somewhere in that area, function 0. */
	for (slot=0;slot < 5 && ret == -ENODEV;slot++) {
		struct pci_dev *dev = pci_get_slot(bus,PCI_DEVFN(slot,0));
		if (!dev) continue;

		if (dev->vendor != 0x8086) {
			DBG_("  PCI slot %d, vendor is not Intel",slot);
			continue;
		}

		if ((dev->class & 0xFF0000) != 0x030000) {
			DBG_("  PCI slot %d is not VGA",slot);
			continue;
		}

		switch (dev->device) {
			case 0x3582:
				chipset = CHIP_855;
				DBG_("  PCI slot %d, found 855 chipset",slot);
				ret = get_855_info(bus,slot);
				break;
		}
	}

	/* if we got an aperature size, figure out how large the table must be */
	if (ret == 0) {
		pgtable_size = (aperature_size >> 12UL) << 2;	/* each page needs 4 bytes */
		DBG_("Page table to cover that aperature needs %u entries x 4 = %u bytes",
			pgtable_entries,pgtable_size);	/* <- WARNING: pgtable_entries is a macro */
	}

	return ret;
}

/* write page table register to change mapping */
static void intel_switch_pgtable(unsigned long addr) {
	DBG_("setting page table control = 0x%08lX",addr);
	MMIO(0x2020) = addr | 1;
}

/* generate a safe pagetable that restores framebuffer sanity.
 * overwrites the contents of pgtable to do it.
 * the result lies in system RAM in a buffer we allocated,
 * but mimicks the layout used by Intel's VGA BIOS (see above for comments) */
static void pgtable_make_default(void) {
	unsigned int page=0,addr=0;
	unsigned int def_sz = intel_stolen_size - pgtable_size;

	DBG_("making default pgtable. pgtable sz=%u",def_sz);

	while (addr < aperature_size && page < pgtable_entries && addr < def_sz) {
		pgtable[page++] = (intel_stolen_base + addr) | 1;
		addr += PAGE_SIZE;
	}

	/* map out page table itself by repeating last entry */
	while (addr < aperature_size && page < pgtable_entries) {
		pgtable[page] = pgtable[page-1];
		addr += PAGE_SIZE;
		page++;
	}

	/* fill rest with zero */
	while (page < pgtable_entries)
		pgtable[page++] = 0;
}

/* like pgtable_make_default() but purposely maps in the page table itself
 * and the system management mode area, so we can "pierce the veil" that
 * Intel's chipsets put in place to normally hide these areas (writes to the
 * "stolen" area and reads from it are terminated by the bridge) */
static void pgtable_pierce_the_veil(void) {
	unsigned int page=0,addr=0;

	DBG("making veil-piercing table");

	while (addr < aperature_size && page < pgtable_entries && addr < (intel_total_memory - intel_stolen_base)) {
		pgtable[page++] = (intel_stolen_base + addr) | 1;
		addr += PAGE_SIZE;
	}

	/* fill rest with zero */
	while (page < pgtable_entries)
		pgtable[page++] = 0;
}

/* generate safe pagetable, and point the Intel chipset at it.
 * after this call, the active framebuffer is our buffer. be careful! */
static void pgtable_default_our_buffer(void) {
	pgtable_make_default();
	intel_switch_pgtable(pgtable_base_phys);
}

/* generate safe pagetable, and point the Intel chipset at it.
 * after this call, the active framebuffer is our buffer. be careful! */
static void pgtable_pierce_the_veil_now(void) {
	pgtable_pierce_the_veil();
	intel_switch_pgtable(pgtable_base_phys);
}

/* chardev file operations */
static ssize_t tvbox_i8xx_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
	return -EIO;
}

static long tvbox_i8xx_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	return -EIO;
}

static int tvbox_i8xx_open(struct inode *inode, struct file *file) {
	return -EBUSY;
}

static int tvbox_i8xx_release(struct inode *inode, struct file *file) {
	return 0;
}

static int tvbox_i8xx_mmap(struct file *file,struct vm_area_struct *vma) {
	return -ENOMEM;
}

static const struct file_operations tvbox_i8xx_fops = {
	.owner          = THIS_MODULE,
	.llseek         = no_llseek,
	.read           = tvbox_i8xx_read,
	.mmap		= tvbox_i8xx_mmap,
	.unlocked_ioctl = tvbox_i8xx_ioctl,
	.open           = tvbox_i8xx_open,
	.release        = tvbox_i8xx_release,
};

static struct miscdevice tvbox_i8xx_dev = {
	.minor			= TVBOX_I8XX_MINOR,
	.name			= "tvbox_i8xx",
	.fops			= &tvbox_i8xx_fops,
};

static int __init tvbox_i8xx_init(void) {
	printk(KERN_INFO "Tv Box v3.0 support driver for Intel 8xx/9xx chipsets "
		"(C) 2009 Jonathan Campbell\n");

	DBG("Scanning for Intel video chipset");
	if (find_intel_graphics()) {
		DBG("Didn't find anything");
		return -ENODEV;
	}

	DBG("Allocating block of physmem");
	if (alloc_pgtable()) {
		DBG("cannot alloc");
		return -ENOMEM;
	}

	DBG("Mapping MMIO");
	if (map_mmio()) {
		free_pgtable();
		DBG("cannot mmap");
		return -ENOMEM;
	}

	DBG_("Registering char dev misc, minor %d",TVBOX_I8XX_MINOR);
	if (misc_register(&tvbox_i8xx_dev)) {
		unmap_mmio();
		free_pgtable();
		DBG("Misc register failed!");
		return -ENODEV;
	}

	DBG("Redirecting screen to my local pagetable, away from VESA BIOS");
	pgtable_default_our_buffer();

	DBG("Piercing the veil");
	pgtable_pierce_the_veil_now();

	return 0; /* OK */
}

static void __exit tvbox_i8xx_cleanup(void) {
	DBG("Unregistering device");
	misc_deregister(&tvbox_i8xx_dev);
	DBG("Freeing pagetable");
	free_pgtable();
	DBG("Unmapping MMIO");
	unmap_mmio();
	DBG("Goodbye");
}

module_init(tvbox_i8xx_init);
module_exit(tvbox_i8xx_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonathan Campbell");
MODULE_DESCRIPTION("tvbox_i8xx filesystem driver");

