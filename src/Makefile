# Out-of-tree / DKMS build of the DACBerry 400 machine driver.
#
# Native on the Pi:   make            (needs linux-headers / raspberrypi-kernel-headers)
# Cross:              make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#
# Module name matches the in-tree driver (snd-soc-dacberry400) so a DKMS
# install into /lib/modules/$(uname -r)/updates overrides it.

obj-m += snd-soc-dacberry400.o
snd-soc-dacberry400-y := dacberry400.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) INSTALL_MOD_DIR=updates modules_install
	depmod -a

.PHONY: all clean install
