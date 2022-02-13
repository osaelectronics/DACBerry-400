obj-m += dacberry400.o

dacberry400.o += -DDEBUG

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KDIR) M=$(PWD) modules

clean:
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KDIR) M=$(PWD) clean
