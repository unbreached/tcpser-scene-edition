.DEFAULT_GOAL := all
SRC=src
SRCS = $(SRC)/bridge.c $(SRC)/debug.c $(SRC)/getcmd.c $(SRC)/ip.c $(SRC)/init.c $(SRC)/modem_core.c $(SRC)/nvt.c $(SRC)/serial.c $(SRC)/ip232.c $(SRC)/util.c $(SRC)/phone_book.c $(SRC)/tcpser.c $(SRC)/line.c $(SRC)/dce.c $(SRC)/analyzer.c $(SRC)/dashboard.c $(SRC)/connlog.c $(SRC)/webhook.c $(SRC)/webserver.c $(SRC)/secure_script.c $(SRC)/secure_paths.c $(SRC)/secure_http.c $(SRC)/web_assets.c $(SRC)/tui.c $(SRC)/options.c
OBJS = $(SRC)/bridge.o $(SRC)/debug.o $(SRC)/getcmd.o $(SRC)/ip.o $(SRC)/init.o $(SRC)/modem_core.o $(SRC)/nvt.o $(SRC)/serial.o $(SRC)/ip232.o $(SRC)/util.o $(SRC)/phone_book.o $(SRC)/tcpser.o $(SRC)/dce.o $(SRC)/line.o $(SRC)/analyzer.o $(SRC)/dashboard.o $(SRC)/connlog.o $(SRC)/webhook.o $(SRC)/webserver.o $(SRC)/secure_script.o $(SRC)/secure_paths.o $(SRC)/secure_http.o $(SRC)/web_assets.o $(SRC)/tui.o $(SRC)/options.o

# Web dashboard static files live in src/web/ and are embedded into the binary
# at build time (POSIX sh + od, no extra tools). Edit the files there, not the
# generated web_assets.c.
WEB_FILES = $(SRC)/web/index.html $(SRC)/web/app.js $(SRC)/web/style.css
$(SRC)/web_assets.c: $(WEB_FILES) tools/embed.sh
	sh tools/embed.sh $@ $(WEB_FILES)
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
# Automatic header dependency tracking (gcc/clang): editing a .h now rebuilds
# every .c that includes it. Previously only the touched .c was recompiled, so
# a struct change in modem_core.h left other objects with a stale layout.
DEPFLAGS = -MMD -MP
DEPS = $(OBJS:.o=.d)

# Install paths (override with: make install PREFIX=/opt/tcpser)
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
DATADIR = $(PREFIX)/share/tcpser
DESTDIR ?=

all:	tcpser

$(SRC)/%.o: $(SRC)/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

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
	install -m 644 data/tcpser.conf $(DESTDIR)$(DATADIR)/
	@echo ""
	@echo "Installed tcpser to $(DESTDIR)$(BINDIR)/tcpser"
	@echo "Data files installed to $(DESTDIR)$(DATADIR)/"
	@echo ""
	@echo "Example:"
	@echo "  tcpser -d /dev/ttyUSB0 -s 2400 -p 6400 -x -P $(DATADIR)/prelogin.seq"
	@echo ""
	@echo "Config file + service (Linux):"
	@echo "  sudo install -d /etc/tcpser && sudo cp $(DATADIR)/tcpser.conf /etc/tcpser/"
	@echo "  edit /etc/tcpser/tcpser.conf, then: tcpser check --config /etc/tcpser/tcpser.conf"
	@echo "  sudo cp $(DATADIR)/tcpser.service /etc/systemd/system/ && sudo systemctl enable --now tcpser"
	@echo "macOS:  sudo cp $(DATADIR)/com.tcpser.bbs.plist /Library/LaunchDaemons/  (edit paths first)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tcpser
	rm -f $(DESTDIR)$(MANDIR)/tcpser.1
	rm -rf $(DESTDIR)$(DATADIR)

depend:
	@echo "Header dependencies are tracked automatically (-MMD); nothing to do."

clean:
	-rm -f tcpser *.bak $(SRC)/*~ $(SRC)/*.o $(SRC)/*.d $(SRC)/*.bak $(SRC)/web_assets.c core
	-rm -rf build

# ---- tests ---------------------------------------------------------------
# make check       : drive the normal binary through a full modem session,
#                    the gates, the web dashboard, CLI validation, pty loss
# make check-asan  : same against an AddressSanitizer+UBSan build in build/;
#                    any sanitizer report fails the run
SAN_CFLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter
check: tcpser
	@sh tools/gen-docs.sh ./tcpser --check || { echo "make check: FAILED (docs out of date)"; exit 1; }
	@python3 tests/run_tests.py ./tcpser || { echo "make check: FAILED"; exit 1; }

# Option tables in README.md / man/tcpser.1 are generated from src/options.c
docs: tcpser
	sh tools/gen-docs.sh ./tcpser

check-docs: tcpser
	sh tools/gen-docs.sh ./tcpser --check

build/tcpser-asan: $(SRCS) $(wildcard $(SRC)/*.h)
	mkdir -p build
	clang $(SAN_CFLAGS) $(DEF) $(SRCS) -o $@ -lpthread

check-asan: build/tcpser-asan
	ASAN_OPTIONS=halt_on_error=0:detect_leaks=0 python3 tests/run_tests.py build/tcpser-asan

.PHONY: all install uninstall depend clean check check-asan docs check-docs

