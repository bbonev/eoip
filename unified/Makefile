# Unified out-of-tree build of the EoIP (eoip.ko) and EoIPv6 (eoipv6.ko)
# tunnel drivers for any supported kernel (3.2 .. 7.0+).  No patching, no
# per-version source selection: the same sources build everywhere.  Both
# modules are standalone - they do not replace the in-tree gre module.
#
#   make            - build against the running kernel
#   make KDIR=...   - build against specific kernel headers
#   make install    - install the modules for the target kernel

ifneq ($(KERNELRELEASE),)

obj-m := eoip.o
ifneq ($(CONFIG_IPV6),)
obj-m += eoipv6.o
endif

else

KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(Q)$(MAKE) --no-print-directory -C $(KDIR) M=$(CURDIR) modules

clean:
	$(Q)$(MAKE) --no-print-directory -C $(KDIR) M=$(CURDIR) clean

install modules_install:
	$(Q)$(MAKE) --no-print-directory -C $(KDIR) M=$(CURDIR) modules_install

uninstall:
	$(E) RMMOD
	$(Q)-modprobe -r eoip
	$(Q)-modprobe -r eoipv6
	$(E) RM eoip/eoipv6 .ko
	$(Q)-rm -f /lib/modules/$(shell uname -r)/updates/eoip.ko*
	$(Q)-rm -f /lib/modules/$(shell uname -r)/updates/eoipv6.ko*
	$(E) DEPMOD
	$(Q)depmod -a

endif
