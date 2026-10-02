# Out-of-tree build of the in-kernel gud driver (Debian kernels ship with CONFIG_DRM_GUD unset)
obj-m := gud.o
gud-y := gud_drv.o gud_pipe.o gud_connector.o
ccflags-y := -I$(src)

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
