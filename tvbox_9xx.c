/* tvbox_i8xx.c
 *
 * kernel-level portion of Tv Box v3.0 rendering engine.
 * handles the icky details of allocating a block of physical RAM
 * that's unbroken and uncached, so that userspace can worry about
 * the rest.
 *
 * We do not concern ourself with the possibility of multiple Intel
 * graphics chipsets, because the graphics device is never really
 * an independent device, it's always an embedded part of the
 * PCI northbridge, and usually at a fixed PCI bus/dev/fn, and
 * there is never more than one. Maybe someday if Intel starts
 * doing weird things like two devices...
 *
 * (C) 2009 Impact Studio Pro.
 *
 * 2009/08/17: don't modify PAGE_CNTL anymore, we seem to be
 *             coming across motherboards with weird values
 *             at startup that imply strange remapping at work.
 *             rather than risk corrupting some region of memory,
 *             it's best to just use the GTT part of the MMIO
 *             register space to update.
 *
 *             Our pgtable is from now on a backup copy, not the
 *             live copy.
 *
 * TODO: This code works great so far on current test hardware. No
 *       visible glitching. Works equally well on x86-64 kernels.
 *
 *       However, there are some important updates that need to be
 *       done:
 *
 *       * Intel 965: The chipset registers are documented to have
 *                    extended fields for systems with 4GB or more
 *                    memory (to support stolen memory from above
 *                    or below the 4GB mark). This code won't work
 *                    on such systems, you first need to add code
 *                    that detects this condition, and fails. When
 *                    you get hardware with Intel 965 and >= 4GB
 *                    update this code to support it properly.
 *
 *       * API to userspace:
 *           [DONE]
 *           - ability to ask for info (top of memory, stolen
 *             memory, etc.)
 *
 *           [DONE]
 *           - ioctl to run our "safe reset" code (on command,
 *             we "pierce the veil" and then write into stolen
 *             RAM a table mimicking what Intel's VGA BIOS creates
 *             after a fresh reboot or mode set). Userspace can
 *             use this as a safe known configuration to start
 *             from when modifying h/w registers.
 *
 *           [DONE]
 *           - ioctl to switch to our allocated table copy
 *
 *           [DONE]
 *           - ability to mmap our page table. we must map into
 *             the process's space pages that allow modification.
 *             the mapping must be "uncacheable", something userspace
 *             cannot accomplish on it's own safely. uncacheable
 *             is a must because today's processors have very deep
 *             caches and queues that would delay any updates userspace
 *             is trying to do.
 *
 *           [DONE]
 *           - safe read/write to/from the page table as fallback
 *             (if we can't get mmap to work---it's slow, but it
 *             works)
 *
 *           [DONE]
 *           - you need to add code preventing access to this device
 *             to anyone except super-user. this is dangerous shit
 *             in the wrong hands, we can't let an accidental
 *             "chmod 0777" allow non-root to trash RAM.
 *
 *           [Nahhhhh...]
 *           - API to mass convert process virtual addresses to physical?
 *             So userspace can pass in a huge array of address and
 *             corresponding process IDs.
 *
 *           [DONE]
 *           - Optional: have a mutex to prevent more than one process
 *             from opening this device.
 */

#include <linux/miscdevice.h>
#include <linux/capability.h>
/*#include <linux/semaphore.h>*/
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

#include "tvbox_9xx.h"

#if defined(DEBUG_ME)
# define DBG_(x,...) printk(KERN_INFO "tvbox_i8xx: " x "\n", __VA_ARGS__ )
# define DBG(x) printk(KERN_INFO "tvbox_i8xx: " x "\n")
#else
# define DBG_(x,...) { }
# define DBG(x) { }
#endif

static unsigned long MB(unsigned long x) { return x << 20UL; }
// static unsigned long KB(unsigned long x) { return x << 10UL; }

/* this is a one-process-at-a-time driver, no concurrent issues that way */
static unsigned int	is_open = 0;
static spinlock_t	lock = SPIN_LOCK_UNLOCKED;

static size_t		pgtable_size = 0;
/* handy way for programmer reference. pgtable_size is in bytes */
#define pgtable_entries (pgtable_size / 4)

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
static size_t		intel_smm_size = 0;

/* Intel PCI device information */
static size_t		aperature_size = 0;
static size_t		aperature_base = 0;	/* first aperature only */
static int		chipset = 0;

/* only the first device's MMIO */
static size_t			mmio_base = 0;
static size_t			mmio_size = 0;
static volatile uint32_t*	mmio = NULL;

#define MMIO(x)			( *( mmio + ((x) >> 2) ) )

/* Intel specicially documents that half the PCI range is the MMIO, and the other half a direct window into the GTT */
#define GTT(x)			MMIO(((x) << 2) + (mmio_size>>1))

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
		DBG_("unmap mmio: 0x%08lX",(unsigned long)mmio);
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
			/* the aperature must exist below 4GB boundary */
			if ((sizeof(res->start) <= 4) || (res->start < 0xFFFF0000 && res->end < 0xFFFF0000)) {
				base = res->start;
				size = (res->end - res->start) + 1;
			}
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

		/* the mmio is the small one that is marked "non-prefetchable" */
		if ((res->flags & IORESOURCE_MEM) && !(res->flags & IORESOURCE_PREFETCH) &&
			!(res->flags & IORESOURCE_DISABLED) && res->start != 0 && base == 0) {
			/* the aperature must exist below 4GB boundary */
			if ((sizeof(res->start) <= 4) || (res->start < 0xFFFF0000 && res->end < 0xFFFF0000)) {
				base = res->start;
				size = (res->end - res->start) + 1;
			}
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

	switch ((w >> 4) & 7) {
		case 1:	intel_stolen_size = MB(1);	break;
		case 2:	intel_stolen_size = MB(4);	break;
		case 3: intel_stolen_size = MB(8);	break;
		case 4:	intel_stolen_size = MB(16);	break;
		case 5:	intel_stolen_size = MB(32);	break;
	}

	/* try to get stolen base */
	{
		uint8_t b;
		pci_bus_read_config_byte(bus,PCI_DEVFN(0,0),0x61,&b);
		DBG_("ESMRAMC 0x%02X",b);
		if (b & 1)	intel_smm_size = MB(1);
		else		intel_smm_size = 0;
		DBG_("SMM area: %uMB",(unsigned int)(intel_smm_size >> 20UL));
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
		intel_stolen_base += MB(32) + intel_stolen_size - 1;
		intel_stolen_base &= ~(MB(32) - 1);

		intel_total_memory = intel_stolen_base;

		intel_stolen_base -= intel_smm_size;	/* System Management Mode area */
		intel_stolen_base -= intel_stolen_size;
	}

	DBG_("Stolen memory: %uMB @ 0x%08lX",(unsigned int)(intel_stolen_size >> 20U),(unsigned long)intel_stolen_base);
	if (intel_stolen_size == 0 || intel_stolen_base == 0)
		return -ENODEV;

	return 0;
}

static int get_965_stolen_memory_info(struct pci_bus *bus) {
	uint16_t w;

	intel_smm_size = 0;
	intel_stolen_base = 0;
	intel_stolen_size = 0;

	/* Host Hub Interface Bridge dev 0 */
	if (pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0x52,&w)) {
		DBG_("Whoah! Cannot read PCI configuration space word @ 0x%X",0x52);
		return -ENODEV;
	}

	DBG_("Intel 965 HHIB CFG word 0x52: 0x%04X",w);

	switch ((w >> 4) & 0x7) {
		case 1:	intel_stolen_size = MB(1);	break;
		case 3: intel_stolen_size = MB(8);	break;
/*		case 5:	intel_stolen_size = MB(32);	break;	undocumented, seen on a motherboard of mine */
	}

	/* the 965 has an explicit register for "top of memory", use that */
	{
		uint16_t w=0;
		uint32_t dw=0;
		uint64_t stolen_base;
		uint64_t total_memory;
		uint64_t total_upper_memory;
		pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0xB0,&w);
		intel_total_memory = (w >> 4) << 20;
		DBG_("Intel TOLUD = 0x%08lX",(unsigned long)intel_total_memory);

		pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0xA0,&w);
		total_memory = ((uint64_t)w) << 26;
		DBG_("Intel TOM = 0x%08llX",(unsigned long long)total_memory);

		pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0xA2,&w);
		total_upper_memory = ((uint64_t)w) << 20;
		DBG_("Intel TOUUD = 0x%08llX",(unsigned long long)total_upper_memory);

		pci_bus_read_config_dword(bus,PCI_DEVFN(0,0),0xA4,&dw);
		stolen_base = ((uint64_t)dw);
		DBG_("Intel GBSM = 0x%08llX",(unsigned long long)stolen_base);

		if (stolen_base == 0) {
			pci_bus_read_config_dword(bus,PCI_DEVFN(2,0),0x5C,&dw);
			DBG_("Intel vid BSM = 0x%08lX",(unsigned long)dw);
			if (dw != 0) stolen_base = dw;
		}

		if (stolen_base != 0) {
			intel_stolen_base = stolen_base;
			intel_stolen_size = intel_total_memory - intel_stolen_base;
		}
		else if (intel_total_memory != 0 && intel_stolen_size != 0)
			intel_stolen_base = intel_total_memory - intel_stolen_size;
	}

	/* take "total ram" estimate from Linux, round up to likely 32MB multiple,
	 * subtract 1MB for the SMM area, and subtract for the stolen base, and...
	 * that's where it starts */
	if (intel_total_memory == 0) {
		struct sysinfo s;
		si_meminfo(&s);
		DBG("TOLUD register worthless, estimating");
		intel_stolen_base = s.totalram * s.mem_unit;
		DBG_("sysinfo: total ram pages %lu mem unit %lu",(unsigned long)s.totalram,(unsigned long)s.mem_unit);

		/* Linux get's it's total memory report from the BIOS, who of course
		 * returns total ram - 1MB - stolen RAM. so we have to round back up
		 * to what is most likely. Intel docs imply the chipset can only handle
		 * amounts of RAM up to the nearest 32MB or 64MB multiple */
		intel_stolen_base += MB(64) + intel_stolen_size - 1;
		intel_stolen_base &= ~(MB(64) - 1);
		intel_total_memory = intel_stolen_base;
		intel_stolen_base -= intel_stolen_size;
	}

	DBG_("Stolen memory: %uMB @ 0x%08lX",(unsigned int)(intel_stolen_size >> 20U),(unsigned long)intel_stolen_base);
	if (intel_stolen_size == 0 || intel_stolen_base == 0)
		return -ENODEV;

	return 0;
}

static int get_855_info(struct pci_bus *bus,int slot) {
	struct pci_dev *primary = pci_get_slot(bus,PCI_DEVFN(slot,0));		/* primary function */
#ifdef USE_SECONDARY
	struct pci_dev *secondary = pci_get_slot(bus,PCI_DEVFN(slot,1));	/* for those with secondary function for second head */
#endif

	if (primary == NULL)
		return -ENODEV;

	/* first, primary device */
	aperature_size = find_intel_aperature(primary,&aperature_base);
	if (aperature_size > 0) {
		DBG_("First aperature: @ 0x%08lX size %08lX",(unsigned long)aperature_base,(unsigned long)aperature_size);

#ifdef USE_SECONDARY
		if (secondary) {
			/* secondary? */
			size_t second_size,second_base;
			second_size = find_intel_aperature(secondary,&second_base);
			if (second_size > 0) {
				DBG_("Second aperature: @ 0x%08lX size %08lX",(unsigned long)second_base,(unsigned long)second_size);
				aperature_size += second_size;
			}
		}
#endif
	}

	/* primary device: get MMIO */
	mmio_size = find_intel_mmio(primary,&mmio_base);
	if (mmio_base != 0 && mmio_size != 0)
		DBG_("First MMIO @ 0x%08lX size %08lX",(unsigned long)mmio_base,(unsigned long)mmio_size);

	if (aperature_size > 0)
		DBG_("Total aperature size: 0x%08lX %uMB",(unsigned long)aperature_size,(unsigned int)(aperature_size >> 20UL));

	if (get_855_stolen_memory_info(bus))
		return -ENODEV;

	return (aperature_base != 0 && aperature_size != 0) ? 0 : -ENODEV;
}

static int get_965_info(struct pci_bus *bus,int slot) {
	struct pci_dev *primary = pci_get_slot(bus,PCI_DEVFN(slot,0));		/* primary function */
#ifdef USE_SECONDARY
	struct pci_dev *secondary = pci_get_slot(bus,PCI_DEVFN(slot,1));	/* for those with secondary function for second head */
#endif

	if (primary == NULL)
		return -ENODEV;

	/* first, primary device */
	aperature_size = find_intel_aperature(primary,&aperature_base);
	if (aperature_size > 0) {
		DBG_("First aperature: @ 0x%08lX size %08lX",(unsigned long)aperature_base,(unsigned long)aperature_size);

#ifdef USE_SECONDARY
		if (secondary) {
			/* secondary? */
			size_t second_size,second_base;
			second_size = find_intel_aperature(secondary,&second_base);
			if (second_size > 0) {
				DBG_("Second aperature: @ 0x%08lX size %08lX",(unsigned long)second_base,(unsigned long)second_size);
				aperature_size += second_size;
			}
		}
#endif
	}

	/* primary device: get MMIO */
	mmio_size = find_intel_mmio(primary,&mmio_base);
	if (mmio_base != 0 && mmio_size != 0)
		DBG_("First MMIO @ 0x%08lX size %08lX",(unsigned long)mmio_base,(unsigned long)mmio_size);

	if (aperature_size > 0)
		DBG_("Total aperature size: 0x%08lX %uMB",(unsigned long)aperature_size,(unsigned int)(aperature_size >> 20UL));

	if (get_965_stolen_memory_info(bus))
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
			case 0x2A02:
			case 0x2e32:
			case 0x2E22:
			case 0x2a42:
				chipset = CHIP_965;
				DBG_("  PCI slot %d, found 965 chipset",slot);
				ret = get_965_info(bus,slot);
				break;
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
			(unsigned int)(pgtable_entries),
			(unsigned int)pgtable_size);	/* <- WARNING: pgtable_entries is a macro */
	}

	return ret;
}

/* set H/W status page address */
static void set_hws_pga(unsigned long addr) {
	DBG_("setting h/w status page = 0x%08lX",addr);
	MMIO(0x2080) = addr & (~0xFFFUL);
}

/* generate a safe pagetable that restores framebuffer sanity.
 * overwrites the contents of pgtable to do it.
 * the result lies in system RAM in a buffer we allocated,
 * but mimicks the layout used by Intel's VGA BIOS (see above for comments) */
static void pgtable_restore(void) {
	unsigned int page=0,addr=0;
	unsigned int def_sz = intel_stolen_size - pgtable_size;

	DBG_("making default pgtable. pgtable sz=%u",def_sz);

	while (addr < aperature_size && page < pgtable_entries && addr < def_sz) {
		GTT(page) = (intel_stolen_base + addr) | 1;
		addr += PAGE_SIZE;
		page++;
	}

	/* map out page table itself by repeating last entry */
	while (addr < aperature_size && page < pgtable_entries) {
		GTT(page) = GTT(page-1);
		addr += PAGE_SIZE;
		page++;
	}

	/* fill rest with zero */
	while (page < pgtable_entries) {
		GTT(page) = 0;
		page++;
	}
}

/* pierce the veil to write into stolen memory, put a replacement table there (as if the Intel VGA BIOS has done it)
 * and then close it back up and walk away. */
static void pgtable_vesa_bios_default(void) {
	pgtable_restore();

	/* restore h/w status register */
	if (intel_stolen_base != 0 && intel_stolen_size != 0)
		set_hws_pga(intel_stolen_base + (intel_stolen_size>>1));	/* <- we have to point it SOMEWHERE */

	/* at this point the contents of our table no longer matter.
	 * that is good---it's a safe default to fall back on so that
	 * userspace counterpart has a good springboard to start with.
	 * may it help uvesafb's job too :) */
}

/* alignment is enforced. partial integers are dropped. we make this obvious by the byte count */
static ssize_t tvbox_i8xx_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
	loff_t pos = *ppos;
	ssize_t ret = 0;
/*	DBG("write"); */

	/* sanity check */
	if (pos & 3)
		return -EINVAL;

	pos >>= 2ULL;
	while (count >= sizeof(uint32_t)) {
		uint32_t word;

		if (pos >= pgtable_entries)
			break;

		if (get_user(word,(uint32_t *)buf)) {
			ret = -EFAULT;
			break;
		}

		GTT(pos++) = word;
		count -= sizeof(uint32_t);
		buf += sizeof(uint32_t);
		ret += sizeof(uint32_t);
	}

	*ppos = pos << 2ULL;
	return ret;
}

static ssize_t tvbox_i8xx_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
	loff_t pos = *ppos;
	ssize_t ret = 0;
/*	DBG("read"); */

	/* sanity check */
	if (pos & 3)
		return -EINVAL;

	pos >>= 2ULL;
	while (count >= sizeof(uint32_t)) {
		uint32_t word;

		if (pos >= pgtable_entries)
			break;

		word = GTT(pos++);
		if (put_user(word,(uint32_t *)buf)) {
			ret = -EFAULT;
			break;
		}

		count -= sizeof(uint32_t);
		buf += sizeof(uint32_t);
		ret += sizeof(uint32_t);
	}

	*ppos = pos << 2ULL;
	return ret;
}

static long tvbox_i8xx_ioctl_ginfo(struct tvbox_i8xx_info __user *u_nfo) {
	struct tvbox_i8xx_info i;
	i.total_memory		= intel_total_memory;
	i.stolen_base		= intel_stolen_base;
	i.stolen_size		= intel_stolen_size;
	i.aperature_base	= aperature_base;
	i.aperature_size	= aperature_size;
	i.mmio_base		= mmio_base;
	i.mmio_size		= mmio_size;
	i.chipset		= chipset;
	i.pgtable_base		= 0;	/* pgtable_base_phys; */
	i.pgtable_size		= pgtable_size;
	i.hwst_base		= 0;	/* hwst_base_phys; */
	i.hwst_size		= 0;	/* PAGE_SIZE; */
	return copy_to_user(u_nfo,&i,sizeof(i));
}

static long tvbox_i8xx_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	int ret = -EIO;
	spin_lock(&lock);
	DBG("ioctl");

	switch (cmd) {
		case TVBOX_I8XX_GINFO:
			ret = tvbox_i8xx_ioctl_ginfo((struct tvbox_i8xx_info __user *)arg);
			break;
		case TVBOX_I8XX_SET_DEFAULT_PGTABLE:
			pgtable_restore();
			ret = 0;
			break;
		case TVBOX_I8XX_SET_VGA_BIOS_PGTABLE:
			pgtable_vesa_bios_default();
			ret = 0;
			break;
		case TVBOX_I8XX_PGTABLE_ACTIVATE:
			ret = 0;
			break;
	}

	spin_unlock(&lock);
	return ret;
}

static int tvbox_i8xx_open(struct inode *inode, struct file *file) {
	/* only God may use this interface */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	spin_lock(&lock);
	if (is_open) {
		spin_unlock(&lock);
		return -EBUSY;
	}

	is_open++;
	spin_unlock(&lock);
	return 0;
}

static int tvbox_i8xx_release(struct inode *inode, struct file *file) {
	spin_lock(&lock);
	if (is_open) {
		/* restore the page table---no questions asked.
		 * or else, risk situations where the videomgr crashes or quit too early
		 * and Linux fbcon is drawing on regions of the aperature mapped to
		 * parts of System RAM that it just mapped other sensitive files into... */
		DBG("char device is being released. restoring page tables");
		pgtable_restore();
		/* okay we're done */
		is_open--;
	}
	spin_unlock(&lock);
	return 0;
}

static int tvbox_i8xx_mmap(struct file *file,struct vm_area_struct *vma) {
	int r=1;

	DBG_("mmap vm_start=0x%08X vm_pgoff=0x%08X",(unsigned int)vma->vm_start,(unsigned int)vma->vm_pgoff);

	vma->vm_flags |= VM_IO;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (r) {
		DBG("mmap fail");
		return -EAGAIN;
	}

	DBG("mmap OK");
	return 0;
}

static loff_t tvbox_i8xx_lseek(struct file *file, loff_t offset, int orig)
{
	loff_t size = (loff_t)pgtable_size;

	/* keep it simple: enforce alignment */
	if (offset & 3)
		return -EINVAL;

	switch (orig) {
		default:
			return -EINVAL;
		case 2:
			offset += size;
			break;
		case 1:
			offset += file->f_pos;
		case 0:
			break;
	}

	if (offset < 0 || offset > size)
		return -EINVAL;

	file->f_pos = offset;
	return file->f_pos;
}

static const struct file_operations tvbox_i8xx_fops = {
	.owner          = THIS_MODULE,
	.llseek         = tvbox_i8xx_lseek,
	.read           = tvbox_i8xx_read,
	.write		= tvbox_i8xx_write,
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

	DBG("Mapping MMIO");
	if (map_mmio()) {
		DBG("cannot mmap");
		return -ENOMEM;
	}

	DBG_("Registering char dev misc, minor %d",TVBOX_I8XX_MINOR);
	if (misc_register(&tvbox_i8xx_dev)) {
		unmap_mmio();
		DBG("Misc register failed!");
		return -ENODEV;
	}

	{
		uint32_t pg = MMIO(0x2020);
		uint32_t hw = MMIO(0x2080);
		DBG_("Intel PGTBL_CTL = 0x%08lX",(unsigned long)pg);
		DBG_("Intel HWS_PGA = 0x%08lX",(unsigned long)hw);
	}

	DBG("Redirecting screen to my local pagetable, away from VESA BIOS");
	pgtable_restore();

	return 0; /* OK */
}

static void __exit tvbox_i8xx_cleanup(void) {
	if (mmio != NULL) {
		DBG("Restoring framebuffer and pagetable");
		pgtable_vesa_bios_default();
	}

	DBG("Unregistering device");
	misc_deregister(&tvbox_i8xx_dev);
	DBG("Unmapping MMIO");
	unmap_mmio();
	DBG("Goodbye");
}

module_init(tvbox_i8xx_init);
module_exit(tvbox_i8xx_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonathan Campbell");
MODULE_DESCRIPTION("tvbox_i8xx filesystem driver");

