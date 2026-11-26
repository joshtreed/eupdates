# eupdates - see LICENSE (GPL-3.0-or-later)
VERSION  ?= 0.2.1
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
CC       ?= cc
CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -pedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -DEUPDATES_VERSION=\"$(VERSION)\"
SCDOC    ?= scdoc

all: eupdates doc/eupdates.1

eupdates: src/eupdates.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $<

doc/eupdates.1: doc/eupdates.1.scd
	$(SCDOC) < $< > $@

install: eupdates doc/eupdates.1
	install -Dm755 eupdates $(DESTDIR)$(BINDIR)/eupdates
	install -Dm644 doc/eupdates.1 $(DESTDIR)$(MANDIR)/man1/eupdates.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/eupdates $(DESTDIR)$(MANDIR)/man1/eupdates.1

test:
	CC="$(CC)" sh tests/run.sh

clean:
	rm -f eupdates doc/eupdates.1

.PHONY: all install uninstall test clean
