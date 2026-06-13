SRC=src
SRCS = $(SRC)/bridge.c $(SRC)/debug.c $(SRC)/getcmd.c $(SRC)/ip.c $(SRC)/init.c $(SRC)/modem_core.c $(SRC)/nvt.c $(SRC)/serial.c $(SRC)/ip232.c $(SRC)/util.c $(SRC)/phone_book.c $(SRC)/tcpser.c $(SRC)/line.c $(SRC)/dce.c $(SRC)/analyzer.c $(SRC)/dashboard.c $(SRC)/connlog.c $(SRC)/webhook.c $(SRC)/webserver.c $(SRC)/secure_script.c $(SRC)/secure_paths.c $(SRC)/secure_http.c
OBJS = $(SRC)/bridge.o $(SRC)/debug.o $(SRC)/getcmd.o $(SRC)/ip.o $(SRC)/init.o $(SRC)/modem_core.o $(SRC)/nvt.o $(SRC)/serial.o $(SRC)/ip232.o $(SRC)/util.o $(SRC)/phone_book.o $(SRC)/tcpser.o $(SRC)/dce.o $(SRC)/line.o $(SRC)/analyzer.o $(SRC)/dashboard.o $(SRC)/connlog.o $(SRC)/webhook.o $(SRC)/webserver.o $(SRC)/secure_script.o $(SRC)/secure_paths.o $(SRC)/secure_http.o
CC = gcc
DEF =
UNAME_S := $(shell uname -s)

# Hardening: stack canaries, fortified libc calls, format-string checks, PIE.
HARDEN_CFLAGS = -fstack-protector-strong -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 \
                -Wformat -Wformat-security -fPIE
CFLAGS = -O2 -g $(DEF) -Wall -Wextra -Wno-unused-parameter $(HARDEN_CFLAGS)

LDFLAGS = -lpthread -fPIE
# Full RELRO + immediate binding are GNU ld features (Linux); harmless to skip
# elsewhere (macOS produces PIE by default).
ifeq ($(UNAME_S),Linux)
LDFLAGS += -pie -Wl,-z,relro,-z,now
endif
DEPEND = makedepend $(DEF) $(CFLAGS)

# Install paths (override with: make install PREFIX=/opt/tcpser)
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
DATADIR = $(PREFIX)/share/tcpser
DESTDIR ?=

all:	tcpser

$(SRC)/%.o: $(SRC)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

tcpser: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

install: tcpser
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 tcpser $(DESTDIR)$(BINDIR)/tcpser
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 man/tcpser.1 $(DESTDIR)$(MANDIR)/tcpser.1
	install -d $(DESTDIR)$(DATADIR)
	install -m 644 data/*.seq $(DESTDIR)$(DATADIR)/
	install -m 644 data/tcpser.blocklist $(DESTDIR)$(DATADIR)/tcpser.blocklist
	install -m 644 data/com.tcpser.bbs.plist $(DESTDIR)$(DATADIR)/
	install -m 644 data/tcpser.service $(DESTDIR)$(DATADIR)/
	@echo ""
	@echo "Installed tcpser to $(DESTDIR)$(BINDIR)/tcpser"
	@echo "Data files installed to $(DESTDIR)$(DATADIR)/"
	@echo ""
	@echo "Example:"
	@echo "  tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x \\"
	@echo "    -P $(DATADIR)/prelogin.seq \\"
	@echo "    -G $(DATADIR)/accessgranted.seq \\"
	@echo "    -O $(DATADIR)/connections.csv"
	@echo ""
	@echo "Service files (edit paths before installing):"
	@echo "  Linux:  sudo cp $(DATADIR)/tcpser.service /etc/systemd/system/"
	@echo "  macOS:  sudo cp $(DATADIR)/com.tcpser.bbs.plist /Library/LaunchDaemons/"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tcpser
	rm -f $(DESTDIR)$(MANDIR)/tcpser.1
	rm -rf $(DESTDIR)$(DATADIR)

depend: $(SRCS)
	$(DEPEND) $(SRCS)

clean:
	-rm -f tcpser *.bak $(SRC)/*~ $(SRC)/*.o $(SRC)/*.bak core

.PHONY: all install uninstall depend clean

# DO NOT DELETE THIS LINE -- make depend depends on it.
