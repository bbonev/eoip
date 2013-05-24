STRIP=strip

all: eoip

eoip: eoipcr.c libnetlink.o
	$(CC) -Wall -Os -o eoip eoipcr.c libnetlink.o
	$(STRIP) eoip

libnetlink.o: libnetlink.c libnetlink.h
	$(CC) -Wall -Os -c libnetlink.c

clean:
	rm -f eoip libnetlink.o
