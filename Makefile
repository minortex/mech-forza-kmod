obj-m += mechrevo-ec.o

KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_USES_CLANG := $(shell grep -q '^CONFIG_CC_IS_CLANG=y' \
	$(KDIR)/include/config/auto.conf 2>/dev/null && echo y)

ifeq ($(KERNEL_USES_CLANG),y)
KBUILD_TOOLCHAIN := LLVM=1
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_TOOLCHAIN) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(KBUILD_TOOLCHAIN) clean
