/* test program: get info */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
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

static int show_info(int fd) {
	struct tvbox_i8xx_info nfo;

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

	close(fd);
	return 0;
}

