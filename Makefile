obj-m += tvbox_i8xx.o

ifndef $(KDIR)
KDIR=/usr/src/linux-2.6.30
#KDIR=/usr/src/linux-2.6.28
#KDIR=/usr/src/linux-2.6.28.8
#KDIR=/mnt/sda1/ext2/usr/src/2.6.28.10
endif

all: tvbox_i8xx.ko test_info

test_info: test_info.c
	gcc -std=c99 -pedantic -Wall -o $@ $+

tvbox_i8xx.ko: tvbox_i8xx.c
	make -C $(KDIR) M=$(PWD) modules

install:
	make -C $(KDIR) M=$(PWD) modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f modules.order test_info

load:
	rmmod tvbox_i8xx || true
	insmod tvbox_i8xx.ko

unload:
	rmmod tvbox_i8xx || true

dev:
	rm -f /dev/tvbox_i8xx
	mknod /dev/tvbox_i8xx c 10 248

