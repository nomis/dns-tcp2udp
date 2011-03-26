.PHONY: all clean

CFLAGS=-Wall -Wextra -Wshadow -D_POSIX_C_SOURCE=200112L -D_ISOC99_SOURCE -D_SVID_SOURCE -D_BSD_SOURCE -D_GNU_SOURCE -O2 -ggdb

all: dns-tcp2udp

clean:
	rm -f dns-tcp2udp
