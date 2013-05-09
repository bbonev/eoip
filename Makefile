STRIP=strip

all: eoip

eoip: eoipcr.c
	$(CC) -Wall -o eoip eoipcr.c
	$(STRIP) eoip

clean:
	rm eoip
