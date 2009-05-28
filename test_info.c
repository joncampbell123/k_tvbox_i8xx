/* test program: get info */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "tvbox_i8xx.h"

static const char *get_chipset_name(unsigned int fd) {
	static char tmp_n[64];

	switch (fd) {
		case CHIP_855:	return "Intel 855";
		case CHIP_965:	return "Intel 965";
	}

	sprintf(tmp_n,"%u?",fd);
	return (const char*)(tmp_n);
}

static struct tvbox_i8xx_info nfo;

static int show_info(int fd) {
	if (ioctl(fd,TVBOX_I8XX_GINFO,&nfo)) {
		fprintf(stderr,"Cannot get info, %s\n",strerror(errno));
		return 1;
	}

	printf("Total memory:          %-5luMB (0x%08lX)\n",nfo.total_memory>>20ULL,nfo.total_memory);
	printf("Stolen:                %-5luKB @ 0x%08lX\n",nfo.stolen_size>>10ULL,nfo.stolen_base);
	printf("Aperature:             %-5luMB @ 0x%08lX\n",nfo.aperature_size>>20ULL,nfo.aperature_base);
	printf("MMIO:                  %-5luKB @ 0x%08lX\n",nfo.mmio_size>>10ULL,nfo.mmio_base);
	printf("Driver pgtable:        %-5luKB @ 0x%08lX\n",nfo.pgtable_size>>10ULL,nfo.pgtable_base);
	printf("Chipset:               %s\n",get_chipset_name(nfo.chipset));

	return 0;
}

static int open_again() {
	int fd2 = open("/dev/tvbox_i8xx",O_RDWR);
	if (fd2 >= 0) {
		close(fd2);
		return 1;
	}

	return 0;
}

static void countdown(int c) {
	while (c > 0) {
		printf("%d... ",c--);
		fflush(stdout);
		sleep(1);
	}
	printf("\n");
}

static int def_pgtable(int fd) {
	int r = ioctl(fd,TVBOX_I8XX_SET_DEFAULT_PGTABLE);
	if (r) fprintf(stderr,"Failed to TVBOX_I8XX_SET_DEFAULT_PGTABLE, %s\n",strerror(errno));
	return r;
}

static int vgabios_pgtable(int fd) {
	int r = ioctl(fd,TVBOX_I8XX_SET_VGA_BIOS_PGTABLE);
	if (r) fprintf(stderr,"Failed to TVBOX_I8XX_SET_VGA_BIOS_PGTABLE, %s\n",strerror(errno));
	return r;
}

static int pgtable_activate(int fd) {
	int r = ioctl(fd,TVBOX_I8XX_PGTABLE_ACTIVATE);
	if (r) fprintf(stderr,"Failed to TVBOX_I8XX_PGTABLE_ACTIVATE, %s\n",strerror(errno));
	return r;
}

int main() {
	int fd = open("/dev/tvbox_i8xx",O_RDWR);
	if (fd < 0) {
		fprintf(stderr,"Cannot open device, %s\n",strerror(errno));
		return 1;
	}

	/* CHECK: I can't open this concurrently, right? */
	if (open_again()) return 2;

	printf("Device open, asking info\n");
	if (show_info(fd)) return 2;

	printf("I'm going to test switching to a default sane pgtable\n");
	countdown(2);
	if (def_pgtable(fd)) return 3;

	printf("I'm going to test switching to VGA BIOS pgtable\n");
	countdown(2);
	if (vgabios_pgtable(fd)) return 3;

	printf("I'm going to make driver's pgtable active again\n");
	countdown(2);
	if (pgtable_activate(fd)) return 3;

	/* test lseek(), make sure aligned one work and unaligned ones fail */
	printf("lseek test in progress\n");
	{
		int x;
		for (x=-1000;x < nfo.pgtable_size*2;x++) {
			int r = lseek(fd,x,SEEK_SET);
			if (r < 0) {
				/* should be in range */
				if (x >= 0 && x <= nfo.pgtable_size) {
					/* should be misaligned */
					if ((x & 3) == 0) {
						fprintf(stderr,"BUG! lseek to %d errored out as '%s', but is aligned\n",x,strerror(errno));
						return 1;
					}
					/* error should be EINVAL */
					if (errno != EINVAL) {
						fprintf(stderr,"BUG! lseek correctly errored offset %d as misaligned, but with wrong error %s\n",x,strerror(errno));
						return 1;
					}
				}
				else {
					if (errno != EINVAL) {
						fprintf(stderr,"BUG! lseek failed offset %d, with wrong error %s\n",x,strerror(errno));
						return 1;
					}
				}
			}
			else {
				/* should be in range */
				if (x >= 0 && x <= nfo.pgtable_size) {
					fprintf(stderr,"BUG! lseek allowed out of range offset %d\n",x);
					return 1;
				}
				/* should be aligned */
				if (x & 3) {
					fprintf(stderr,"BUG! lseek allowed misaligned offset %d\n",x);
					return 1;
				}
			}
		}
	}
	printf("lseek passed\n");

	/* test: read the pagetable using lseek+read. purposely try to read beyond the EOF to see if there are bugs there */
	{
		int x;
		uint32_t word=0;
		for (x=0;x < nfo.pgtable_size;x += 4) {
			if (lseek(fd,x,SEEK_SET) != x) {
				fprintf(stderr,"BUG! lseek(%d) != %d\n",x,x);
				return 1;
			}

			int r = read(fd,&word,sizeof(word));
			if (r < 0) {
				fprintf(stderr,"BUG! read from offset %d failed error %s\n",x,strerror(errno));
				return 1;
			}
			else if (r != sizeof(word)) {
				fprintf(stderr,"BUG! read from offset %d worked but only %d bytes read\n",x,r);
				return 1;
			}

			if (word != 0)
				printf("%lu: 0x%08lX\n",(unsigned long)(x>>2),(unsigned long)word);
		}
		
		/* make sure lseek(end) == end but we can't read */
		if (lseek(fd,nfo.pgtable_size,SEEK_SET) != nfo.pgtable_size) {
			fprintf(stderr,"BUG! lseek(end) != end\n");
			return 1;
		}

		if (read(fd,&word,sizeof(word)) != 0) {
			fprintf(stderr,"BUG! I can read at EOF\n");
			return 1;
		}
	}

	/* multiple read test: see if we can pass in an array of uint32_t and get back corresponding pages */
	{
		uint32_t words[256];
		int x,i;

		for (x=0;x < nfo.pgtable_size;x += sizeof(words)) {
			if (lseek(fd,x,SEEK_SET) != x) {
				fprintf(stderr,"BUG: lseek(%d) != %d\n",x,x);
				return 1;
			}

			int r = read(fd,words,sizeof(words));
			if (r < 0) {
				fprintf(stderr,"BUG! read from offset %d failed error %s\n",x,strerror(errno));
				return 1;
			}
			else if (r != sizeof(words) && (x+r) <= nfo.pgtable_size) {
				fprintf(stderr,"BUG! read from offset %d incomplete\n",x);
				return 1;
			}

			for (i=0;i < (r/sizeof(uint32_t));i++) {
				uint32_t word = words[i];
				if (word != 0)
					printf("%lu: 0x%08lX\n",
						(unsigned long)((x>>2)+i),
						(unsigned long)word);
			}
		}
	}

	printf("I'm going to repeat the first page table entry across all.\n");
	printf("Everything should look like vertical streakiness for the time\n");
	countdown(3);

	/* now write to the table.
	 * purposely repeat the first entry so the screen has visible vertical streaks */
	{
		uint32_t w;
		unsigned int x;

		lseek(fd,0,SEEK_SET);
		read(fd,&w,sizeof(w));
		printf("Repeating 0x%08lX\n",(unsigned long)w);

		for (x=0;x < (nfo.pgtable_size/sizeof(uint32_t));x++) {
			if (lseek(fd,x*4,SEEK_SET) != (x*4)) {
				fprintf(stderr,"BUG: lseek(%d) != %d\n",x*4,x*4);
				return 1;
			}

			if (write(fd,&w,sizeof(w)) != sizeof(w)) {
				fprintf(stderr,"Cannot write entry %d\n",x);
				return 1;
			}
		}
	}
	sleep(1);
	{
		uint32_t w,nw;
		unsigned int x;

		lseek(fd,0,SEEK_SET);
		read(fd,&w,sizeof(w));
		printf("Repeating 0x%08lX\n",(unsigned long)w);

		for (x=0;x < (nfo.pgtable_size/sizeof(uint32_t));x++) {
			if (lseek(fd,x*4,SEEK_SET) != (x*4)) {
				fprintf(stderr,"BUG: lseek(%d) != %d\n",x*4,x*4);
				return 1;
			}

			nw = w + ((x>>1) * 4096);

			if (write(fd,&nw,sizeof(w)) != sizeof(w)) {
				fprintf(stderr,"Cannot write entry %d\n",x);
				return 1;
			}
		}
	}
	sleep(1);
	{
		uint32_t w,nw;
		unsigned int x;

		lseek(fd,0,SEEK_SET);
		read(fd,&w,sizeof(w));
		printf("Repeating 0x%08lX\n",(unsigned long)w);

		for (x=0;x < (nfo.pgtable_size/sizeof(uint32_t));x++) {
			if (lseek(fd,x*4,SEEK_SET) != (x*4)) {
				fprintf(stderr,"BUG: lseek(%d) != %d\n",x*4,x*4);
				return 1;
			}

			nw = w + ((x&7) * 4096);

			if (write(fd,&nw,sizeof(w)) != sizeof(w)) {
				fprintf(stderr,"Cannot write entry %d\n",x);
				return 1;
			}
		}
	}
	sleep(1);

        if (def_pgtable(fd)) return 3;

	printf("Going to memory-map it now...\n");
	countdown(3);

	{
		int i;
		volatile uint32_t *x = (volatile uint32_t*)
			mmap(NULL,nfo.pgtable_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
		if (x == (volatile uint32_t*)(-1)) {
			fprintf(stderr,"mmap failed\n");
			return 1;
		}

		printf("Mapped to 0x%08lX (VM)\n",(unsigned long)x);
		for (i=0;i < nfo.pgtable_size/sizeof(uint32_t);i++) {
			uint32_t word = x[i];
			if (word != 0)
				printf("%lu: 0x%08lX\n",
					(unsigned long)i,
					(unsigned long)word);
		}

		{
			uint32_t fw = x[0];

			for (i=0;i < nfo.pgtable_size/sizeof(uint32_t);i++)
				x[i] = fw + ((i&1) * 4096);

			sleep(1);

			for (i=0;i < nfo.pgtable_size/sizeof(uint32_t);i++)
				x[i] = fw + ((i&3) * 4096);

			sleep(1);

			for (i=0;i < nfo.pgtable_size/sizeof(uint32_t);i++)
				x[i] = fw + ((i&7) * 4096);

			sleep(1);

			for (i=0;i < nfo.pgtable_size/sizeof(uint32_t);i++)
				x[i] = fw + (i * 4096);
		}

		munmap((void*)x,nfo.pgtable_size);
	}

	close(fd);
	return 0;
}

