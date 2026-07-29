# use this to disable flto optimizations:
#   make NO_FLTO=1
# and this to enable verbose mode:
#   make V=1
# use this to disable kernel modules build/install/uninstall
#   make NO_MODS=1

ifndef NO_FLTO
CFLAGS?=-O3 -fno-stack-protector -flto
LDFLAGS+=-O3 -fno-stack-protector -flto
else
CFLAGS?=-O3 -fno-stack-protector
endif

STRIP?=strip
SCAN-BUILD?=scan-build-23
PREFIX?=$(DESTDIR)/

MYCFLAGS:=$(CPPFLAGS) $(CFLAGS) -std=gnu90 -Wall -Wextra
MYLDFLAGS=$(LDFLAGS)

ifeq ("$(V)","1")
Q:=
E:=@true
else
Q:=@
E:=@echo
endif

ifndef NO_MODS
all: eoip modules
else
all: eoip
endif

eoip: eoipcr.o libnetlink.o
	$(E) LD $@
	$(Q)$(CC) -o $@ $^ $(MYLDFLAGS)

libnetlink.o: libnetlink.c libnetlink.h
	$(E) CC $@
	$(Q)$(CC) $(MYCFLAGS) -c -o $@ $<

eoipcr.o: eoipcr.c libnetlink.h eoip_version.h unified/eoip_proto.h
	$(E) CC $@
	$(Q)$(CC) $(MYCFLAGS) -c -o $@ $<

ifndef NO_MODS
install: eoip minstall
else
install: eoip
endif
	$(E) STRIP eoip
	$(Q)$(STRIP) eoip
	$(E) INSTALL eoip
	$(Q)install -TD -m 0755 eoip $(PREFIX)sbin/eoip

ifndef NO_MODS
uninstall: muninstall
else
uninstall:
endif
	$(E) UNINSTALL eoip
	$(Q)rm $(PREFIX)sbin/eoip

ifndef NO_MODS
clean: mclean
else
clean:
endif
	$(E) CLEAN
	$(Q)-dh_clean
	$(Q)rm -f eoip eoipcr.o libnetlink.o

re:
	$(Q)$(MAKE) --no-print-directory clean
	$(Q)$(MAKE) --no-print-directory -j all

# scan-build can not easily handle the modules code
scan:
	$(Q)$(MAKE) --no-print-directory clean NO_MODS=1
	$(E) SCAN
	$(Q)$(SCAN-BUILD) $(MAKE) --no-print-directory -j NO_MODS=1 all

# the unified source builds on any supported kernel (3.2 .. 7.0+);
# override KDIR to build for a kernel other than the running one
modules:
	$(E) MAKE MOD
	$(Q)$(MAKE) --no-print-directory -C unified $(if $(KDIR),KDIR=$(KDIR)) Q=$(Q) E=$(E)
	$(Q)cp unified/eoip.ko .
	$(Q)test ! -f unified/eoipv6.ko || cp unified/eoipv6.ko .

mclean:
	$(E) CLEAN MOD
	$(Q)$(MAKE) --no-print-directory -C unified clean $(if $(KDIR),KDIR=$(KDIR)) Q=$(Q) E=$(E)
	$(Q)rm -f eoip.ko
	$(Q)rm -f eoipv6.ko

minstall:
	$(E) INSTALL MOD
	$(Q)$(MAKE) --no-print-directory -C unified modules_install $(if $(KDIR),KDIR=$(KDIR)) Q=$(Q) E=$(E)

muninstall:
	$(E) UNINSTALL MOD
	$(Q)$(MAKE) --no-print-directory -C unified uninstall $(if $(KDIR),KDIR=$(KDIR)) Q=$(Q) E=$(E)

VER:=$(shell ./version.sh)
mkotar:
	$(E) CLEAN
	$(Q)$(MAKE) --no-print-directory clean
	$(E) TAR
	$(Q)tar \
		--xform 's,^[.],eoip-$(VER),' \
		--exclude ./.git \
		--exclude ./.gitignore \
		--exclude ./debian \
		-Jcvf ../eoip_$(VER).orig.tar.xz .
	$(Q)-rm -f ../eoip_$(VER).orig.tar.xz.asc
	$(E) GPG
	$(Q)gpg -a --detach-sign ../eoip_$(VER).orig.tar.xz
	$(Q)cp -fa ../eoip_$(VER).orig.tar.xz ../eoip-$(VER).tar.xz
	$(Q)cp -fa ../eoip_$(VER).orig.tar.xz.asc ../eoip-$(VER).tar.xz.asc

.PHONY: all install uninstall clean re scan modules mclean minstall muninstall mkotar
