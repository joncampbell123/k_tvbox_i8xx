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

#include "tvbox_i8xx.h"

#if defined(DEBUG_ME)
# define DBG_(x,...) printk(KERN_INFO "tvbox_i8xx: " x "\n", __VA_ARGS__ )
# define DBG(x) printk(KERN_INFO "tvbox_i8xx: " x "\n")
#else
# define DBG_(x,...) { }
# define DBG(x) { }
#endif

static unsigned long MB(unsigned long x) { return x << 20UL; }
static unsigned long KB(unsigned long x) { return x << 10UL; }

/* this is a one-process-at-a-time driver, no concurrent issues that way */
static unsigned int	is_open = 0;
static spinlock_t	lock = SPIN_LOCK_UNLOCKED;

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
		set_memory_wb(pgtable_base,pgtable_size >> PAGE_SHIFT);
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
	DBG_("pagetable: Physical memory location 0x%08lX",(unsigned long)pgtable_base_phys);
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

/* for safety's sake we also maintain for userspace the 4KB page associated with the Hardware Status Page.
 * if we let userspace point it at itself, we risk memory corruption in the event the program terminates
 * without resetting it (whatever takes it's place is overwritten with status) */
static unsigned long	hwst_base = 0;
static unsigned long	hwst_base_phys = 0;

void free_hwst_page(void) {
	if (hwst_base != 0) {
		free_page((unsigned long)hwst_base);
		hwst_base_phys = 0;
		hwst_base = 0;
	}
}

int alloc_hwst_page(void) {
	if (hwst_base == 0) {
		hwst_base = __get_free_page(GFP_KERNEL);
		if (hwst_base == 0)
			return 1;

		hwst_base_phys = virt_to_phys((void*)hwst_base);
		DBG_("alloc hardware status page @ 0x%08lX",(unsigned long)hwst_base_phys);
	}

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

	switch ((w >> 4) & 7) {
		case 1:	intel_stolen_size = MB(1);	break;
		case 3: intel_stolen_size = MB(8);	break;
	}

	/* the 965 has an explicit register for "top of memory", use that */
	{
		uint16_t w=0;
		pci_bus_read_config_word(bus,PCI_DEVFN(0,0),0xB0,&w);
		intel_total_memory = (w >> 4) << 20;
		DBG_("Intel TOLUD = 0x%08lX",(unsigned long)intel_total_memory);

		if (intel_total_memory != 0)
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

/* write page table register to change mapping */
static void intel_switch_pgtable(unsigned long addr) {
	uint32_t other = 0;

	if (chipset == CHIP_965) {
		/* 965 also wants the size of the page table.
		 * this is a must especially when pointing to the fake
		 * recreation of VESA BIOS page table at top of RAM.
		 * If the chipset thinks our table extends past the top
		 * it won't use it and the user sees random garbage on their display. */
		if (pgtable_size >= KB(2048))
			other = 4 << 1;
		else if (pgtable_size >= KB(1024+512))
			other = 5 << 1;
		else if (pgtable_size >= KB(1024))
			other = 3 << 1;
		else if (pgtable_size >= KB(512))
			other = 0 << 1;
		else if (pgtable_size >= KB(256))
			other = 1 << 1;
		else
			other = 2 << 1;	/* 128KB */
	}

	DBG_("setting page table control = 0x%08lX",addr | other | 1);
	MMIO(0x2020) = addr | other | 1;
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
static void pgtable_make_default(volatile uint32_t *pgt) {
	unsigned int page=0,addr=0;
	unsigned int def_sz = intel_stolen_size - pgtable_size;

	DBG_("making default pgtable. pgtable sz=%u",def_sz);

	while (addr < aperature_size && page < pgtable_entries && addr < def_sz) {
		pgt[page++] = (intel_stolen_base + addr) | 1;
		addr += PAGE_SIZE;
	}

	/* map out page table itself by repeating last entry */
	while (addr < aperature_size && page < pgtable_entries) {
		pgt[page] = pgtable[page-1];
		addr += PAGE_SIZE;
		page++;
	}

	/* fill rest with zero */
	while (page < pgtable_entries)
		pgt[page++] = 0;
}

/* like pgtable_make_default() but purposely maps in the page table itself
 * and the system management mode area, so we can "pierce the veil" that
 * Intel's chipsets put in place to normally hide these areas (writes to the
 * "stolen" area and reads from it are terminated by the bridge).
 *
 * this is typically used by the code to open up the stolen area, write in
 * a replacement table, redirect to that, and then leave the system in a state
 * as if the Intel VGA BIOS had done it. */
static void pgtable_make_pierce_the_veil(volatile uint32_t *pgt) {
	unsigned int page=0,addr=0;

	DBG("making veil-piercing table");

#if 0
	/* no, don't risk security by exposing SMM memory... */
	while (addr < aperature_size && page < pgtable_entries && addr < (intel_total_memory - intel_stolen_base)) {
#else
	while (addr < aperature_size && page < pgtable_entries && addr < intel_stolen_size) {
#endif
		pgt[page++] = (intel_stolen_base + addr) | 1;
		addr += PAGE_SIZE;
	}

	/* fill rest with zero */
	while (page < pgtable_entries)
		pgt[page++] = 0;
}

/* generate safe pagetable, and point the Intel chipset at it.
 * after this call, the active framebuffer is our buffer. be careful! */
static void pgtable_default_our_buffer(void) {
	pgtable_make_default(pgtable);
	intel_switch_pgtable(pgtable_base_phys);

	/* direct h/w status to our buffer */
	if (intel_stolen_base != 0 && intel_stolen_size != 0 && hwst_base != 0) {
		/* clear it first */
		memset((char*)hwst_base,0,PAGE_SIZE);
		set_hws_pga(hwst_base_phys);
	}
}

/* generate safe pagetable, and point the Intel chipset at it.
 * after this call, the active framebuffer is our buffer. be careful! */
static void pgtable_pierce_the_veil(void) {
	pgtable_make_pierce_the_veil(pgtable);
	intel_switch_pgtable(pgtable_base_phys);
}

/* pierce the veil to write into stolen memory, put a replacement table there (as if the Intel VGA BIOS has done it)
 * and then close it back up and walk away. */
static void pgtable_vesa_bios_default(void) {
	unsigned long vesa_bios_pgtable_offset = intel_stolen_size - (pgtable_size + 0x4000);	/* make room for other structs */

	pgtable_pierce_the_veil();

	DBG("veil pierced, writing replacement table up in stolen area");

	/* stolen mem area open, write in a replacement table */
	{
		volatile uint32_t *npt;
		unsigned long pho = aperature_base + vesa_bios_pgtable_offset;
		DBG_("writing to aperature @ 0x%08lX + 0x%08lX = 0x%08lX",(unsigned long)aperature_base,
			(unsigned long)vesa_bios_pgtable_offset,pho);

		npt = (volatile uint32_t*)ioremap(pho,pgtable_size);
		if (npt == NULL) {
			DBG("Shit! Cannot ioremap that area! Leaving it as-is for safety");
			return;
		}
		pgtable_make_default(npt);
		iounmap((void*)npt);
	}

	/* now switch pagetable to THAT */
	intel_switch_pgtable(intel_stolen_base + vesa_bios_pgtable_offset);

	/* restore h/w status register */
	if (intel_stolen_base != 0 && intel_stolen_size != 0)
		set_hws_pga(intel_stolen_base + intel_stolen_size - 4096);

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

		pgtable[pos++] = word;
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

		word = pgtable[pos++];
/*		DBG_("read @ %lu: 0x%08lX",(unsigned long)(pos << 2ULL),(unsigned long)word); */
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
	i.pgtable_base		= pgtable_base_phys;
	i.pgtable_size		= pgtable_size;
	i.hwst_base		= hwst_base_phys;
	i.hwst_size		= PAGE_SIZE;
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
			pgtable_default_our_buffer();
			ret = 0;
			break;
		case TVBOX_I8XX_SET_VGA_BIOS_PGTABLE:
			pgtable_vesa_bios_default();
			ret = 0;
			break;
		case TVBOX_I8XX_PGTABLE_ACTIVATE:
			intel_switch_pgtable(pgtable_base_phys);
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
	is_open--;
	spin_unlock(&lock);
	return 0;
}

static int tvbox_i8xx_mmap(struct file *file,struct vm_area_struct *vma) {
	size_t size = vma->vm_end - vma->vm_start;
	int r=1;

	DBG_("mmap vm_start=0x%08X vm_pgoff=0x%08X",(unsigned int)vma->vm_start,(unsigned int)vma->vm_pgoff);

	vma->vm_flags |= VM_IO;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (vma->vm_pgoff == (pgtable_base_phys >> PAGE_SHIFT)) {
		if (size <= pgtable_size)
			r = io_remap_pfn_range(vma, vma->vm_start, pgtable_base_phys >> PAGE_SHIFT, size, vma->vm_page_prot);
	}
	else if (vma->vm_pgoff == (hwst_base_phys >> PAGE_SHIFT)) {
		if (size <= PAGE_SIZE)
			r = io_remap_pfn_range(vma, vma->vm_start, hwst_base_phys >> PAGE_SHIFT, PAGE_SIZE, vma->vm_page_prot);
	}

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

	DBG("Allocating block of physmem");
	if (alloc_pgtable()) {
		DBG("cannot alloc");
		return -ENOMEM;
	}

	DBG("Allocating hw status page");
	if (alloc_hwst_page()) {
		DBG("cannot alloc");
		free_pgtable();
		return -ENOMEM;
	}

	DBG("Mapping MMIO");
	if (map_mmio()) {
		free_pgtable();
		free_hwst_page();
		DBG("cannot mmap");
		return -ENOMEM;
	}

	DBG_("Registering char dev misc, minor %d",TVBOX_I8XX_MINOR);
	if (misc_register(&tvbox_i8xx_dev)) {
		unmap_mmio();
		free_pgtable();
		free_hwst_page();
		DBG("Misc register failed!");
		return -ENODEV;
	}

	DBG_("Before init, page table control: 0x%08X",MMIO(0x2020));
	DBG_("and h/w status @ 0x%08X",MMIO(0x2080));

	DBG("Redirecting screen to my local pagetable, away from VESA BIOS");
	pgtable_default_our_buffer();

	return 0; /* OK */
}

static void __exit tvbox_i8xx_cleanup(void) {
	if (pgtable != NULL && mmio != NULL) {
		DBG("Restoring framebuffer and pagetable");
		pgtable_vesa_bios_default();
	}

	DBG("Unregistering device");
	misc_deregister(&tvbox_i8xx_dev);
	DBG("Freeing pagetable");
	free_pgtable();
	DBG("Freeing hwst");
	free_hwst_page();
	DBG("Unmapping MMIO");
	unmap_mmio();
	DBG("Goodbye");
}

module_init(tvbox_i8xx_init);
module_exit(tvbox_i8xx_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonathan Campbell");
MODULE_DESCRIPTION("tvbox_i8xx filesystem driver");

