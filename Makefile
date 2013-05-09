STRIP=strip

all: eoip

eoip: eoipcr.c libnetlink.o
	$(CC) -Wall -o eoip eoipcr.c libnetlink.o
	$(STRIP) eoip

libnetlink.o: libnetlink.c libnetlink.h
	$(CC) -Wall -c libnetlink.c

clean:
	rm -f eoip get_if_list libnetlink.o
