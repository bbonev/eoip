# use this to disable flto optimizations:
#   make NO_FLTO=1
# and this to enable verbose mode:
#   make V=1

TARGET=eoip

ifndef NO_FLTO
CFLAGS?=-O3 -fno-stack-protector -flto
LDFLAGS+=-O3 -fno-stack-protector -flto
else
CFLAGS?=-O3 -fno-stack-protector
endif

STRIP?=strip
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

all: $(TARGET)

eoip: eoipcr.o libnetlink.o
	$(E) LD $@
	$(Q)$(CC) -o $@ $^ $(MYLDFLAGS)

libnetlink.o: libnetlink.c libnetlink.h
	$(E) CC $@
	$(Q)$(CC) $(MYCFLAGS) -c -o $@ $<

eoipcr.o: eoipcr.c libnetlink.h version.h
	$(E) CC $@
	$(Q)$(CC) $(MYCFLAGS) -c -o $@ $<

install: $(TARGET)
	$(E) STRIP $(TARGET)
	$(Q)$(STRIP) $(TARGET)
	$(E) INSTALL $(TARGET)
	$(Q)install -TD -m 0755 $(TARGET) $(PREFIX)sbin/$(TARGET)

uninstall:
	$(E) UNINSTALL $(TARGET)
	$(Q)rm $(PREFIX)sbin/$(TARGET)

clean:
	rm -f eoip eoipcr.o libnetlink.o
