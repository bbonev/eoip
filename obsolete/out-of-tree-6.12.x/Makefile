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
eoip.c: ../kernel-patch/kernel-6.12.0-eoip.patch
	@patch -p3 < $<
gre.c gre.h: ../kernel-patch/kernel-6.12.0-eoip-gre-demux.patch
	@if [ -f /lib/modules/`uname -r`/build/net/ipv4/gre.c ]; then echo found running kernel version of gre.c; cp /lib/modules/`uname -r`/build/net/ipv4/gre_demux.c gre_demux.c; else echo using 6.12.0 version of gre_demux.c; cp gre_demux-6.12.0.c gre_demux.c; fi
	@if [ -f /lib/modules/`uname -r`/build/include/net/gre.h ]; then echo found running kernel version of gre.h; cp /lib/modules/`uname -r`/build/include/net/gre.h gre.h; else echo using 6.12.0 version of gre.h; cp gre-6.12.0.h gre.h; fi
	@patch -p3 < $<
	@cp gre_demux.c gre.c
clean:
	@rm -f gre_demux.c gre.c gre.h eoip.c *.orig *.rej
	@$(MAKE) -C $(KDIR) M=$$PWD clean
install modules_install:
	@$(MAKE) -C $(KDIR) M=$$PWD modules_install
endif
