CC = gcc
CFLAGS = -std=c99 -Os -Wall -Wextra -s -fstack-protector-strong \
         -D_FORTIFY_SOURCE=2 -fPIE -Wl,-z,relro,-z,now
PREFIX = /usr/local

BINARY = nobszram
CONF = nobszram.conf
CONF_DIR = /etc/nobszram

all: $(BINARY)

$(BINARY): $(BINARY).c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINARY)

install: $(BINARY)
	install -d -m 755 $(DESTDIR)$(PREFIX)/bin
	install -d -m 700 $(DESTDIR)$(CONF_DIR)
	install -m 755 $(BINARY) $(DESTDIR)$(PREFIX)/bin/$(BINARY)
	install -m 600 $(CONF) $(DESTDIR)$(CONF_DIR)/$(CONF)
	chown -v root:root $(DESTDIR)$(CONF_DIR)/$(CONF) || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BINARY)
	rm -f $(DESTDIR)$(CONF_DIR)/$(CONF)
	rmdir $(DESTDIR)$(CONF_DIR) || echo "Directory $(DESTDIR)$(CONF_DIR) not empty or does not exist."

.PHONY: all clean install uninstall status
