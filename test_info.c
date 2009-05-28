/* test program: get info */
#include <sys/ioctl.h>
#include <sys/types.h>
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
//	countdown(3);
	if (def_pgtable(fd)) return 3;

	printf("I'm going to test switching to VGA BIOS pgtable\n");
//	countdown(3);
	if (vgabios_pgtable(fd)) return 3;

	printf("I'm going to make driver's pgtable active again\n");
//	countdown(3);
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

	/* test: read the pagetable using lseek+read */
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

			printf("%lu: 0x%08lX\n",(unsigned long)(x>>2),(unsigned long)word);
		}
	}

	close(fd);
	return 0;
}

