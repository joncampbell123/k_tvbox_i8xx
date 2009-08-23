obj-m += tvbox_9xx.o

ifndef $(KDIR)
KDIR=/usr/src/linux-2.6.30
#KDIR=/usr/src/linux-2.6.28
#KDIR=/usr/src/linux-2.6.28.8
#KDIR=/mnt/sda1/ext2/usr/src/2.6.28.10
endif

all: tvbox_9xx.ko test_info

test_info: test_info.c
	gcc -std=c99 -pedantic -Wall -o $@ $+

tvbox_9xx.ko: tvbox_9xx.c
	make -C $(KDIR) M=$(PWD) modules

install:
	make -C $(KDIR) M=$(PWD) modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f modules.order test_info

load:
	rmmod tvbox_9xx || true
	insmod tvbox_9xx.ko

unload:
	rmmod tvbox_9xx || true

dev:
	mkdir -p $(ROOT)/dev
	rm -f $(ROOT)/dev/tvbox_9xx
	mknod $(ROOT)/dev/tvbox_9xx c 10 248

