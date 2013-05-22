ifneq ($(KERNELRELEASE),)
# ugly and dirty hack to avoid patching system's kernel headers
ccflags-y += "-DGREPROTO_NONSTD_EOIP=0x80"
ccflags-y += "-DGREPROTO_NONSTD_MAX=0x81"
ccflags-y += "-DGREPROTO_NONSTD_BASE=0x80"
obj-m  := eoip.o
obj-m  += gre.o
else
KDIR ?= /lib/modules/`uname -r`/build
default: eoip.c gre.c gre.h
	@$(MAKE) -C $(KDIR) M=$$PWD
eoip.c: ../kernel-patch/kernel-3.2.44-eoip.patch
	@patch -p3 < $<
gre.c gre.h: ../kernel-patch/kernel-3.2.44-eoip-gre-demux.patch
	@if [ -f /lib/modules/`uname -r`/build/net/ipv4/gre.c ]; then echo found running kernel version of gre.c; cp /lib/modules/`uname -r`/build/net/ipv4/gre.c gre.c; else echo using 3.2.44 version of gre.c; cp gre-3.2.44.c gre.c; fi
	@if [ -f /lib/modules/`uname -r`/build/include/net/gre.h ]; then echo found running kernel version of gre.h; cp /lib/modules/`uname -r`/build/include/net/gre.h gre.h; else echo using 3.2.44 version of gre.h; cp gre-3.2.44.h gre.h; fi
	@patch -p3 < $<
clean:
	@rm -f gre.c gre.h eoip.c
	@$(MAKE) -C $(KDIR) M=$$PWD clean
install modules_install:
	@$(MAKE) -C $(KDIR) M=$$PWD modules_install
endif
