all: eoip

eoip: eoipcr.c
	$(CC) -Wall -o eoip eoipcr.c

clean:
	rm eoip
