# SPDX-License-Identifier: GPL-2.0
#ccflags-y += -I$(src)/ -DWILC_DEBUGFS
ccflags-y += -I$(src)/ 

wilc-objs := cfg80211.o netdev.o mon.o \
			hif.o wlan_cfg.o wlan.o sysfs.o power.o bt.o debugfs.o

obj-$(CONFIG_WILC_SDIO) += wilc-sdio.o
wilc-sdio-objs += $(wilc-objs)
wilc-sdio-objs += sdio.o

obj-$(CONFIG_WILC_SPI) += wilc-spi.o
wilc-spi-objs += $(wilc-objs)
wilc-spi-objs += spi.o

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules_install

clean:
	rm -f *.o *~ core .depend .*.cmd *.ko *.mod.c
	rm -f Module.markers Module.symvers modules.order
	rm -rf .tmp_versions Modules.symversa
