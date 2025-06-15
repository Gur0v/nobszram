CC=gcc
CFLAGS=-std=c99 -Os -Wall -Wextra -s -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Wl,-z,relro,-z,now
PREFIX=/usr/local

nobszram: nobszram.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f nobszram

install: nobszram
	install -Dm755 nobszram $(DESTDIR)$(PREFIX)/bin/nobszram
	install -Dm644 nobszram.conf $(DESTDIR)/etc/nobszram/nobszram.conf
