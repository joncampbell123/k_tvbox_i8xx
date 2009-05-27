obj-m += tvbox_i8xx.o

ifndef $(KDIR)
KDIR=/mnt/sdb1/ext2/usr/src/2.6.28.10
endif

all:
	make -C $(KDIR) M=$(PWD) modules

install:
	make -C $(KDIR) M=$(PWD) modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f modules.order

load:
	rmmod tvbox_i8xx || true
	insmod tvbox_i8xx.ko

unload:
	rmmod tvbox_i8xx || true

dev:
	rm -f /dev/tvbox_i8xx
	mknod /dev/tvbox_i8xx c 10 240

