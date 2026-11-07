# TCPSER LINK ANALYZER — C64 side build
# Requires: cc65 (ca65/ld65) and VICE (x64sc, c1541).
#
#   make            build tcpser-link.prg and tcpser-link.d64
#   make run        build + launch in VICE (x64sc), no ACIA
#   make shot       build + headless VICE render to shot.png (no 6551 state)
#   make shot-live  headless render of a live session against ../tcpser over ip232
#   make run-live   interactive VICE session against ../tcpser over ip232

PROG   = tcpser-link
CFG    = c64prg.cfg
CA65   = ca65
LD65   = ld65
C1541  = c1541
X64    = x64sc

# NOTE: assemble WITHOUT '-t c64' so string literals stay raw ASCII/PETSCII
# ($41-$5A) instead of being remapped into the $C1-$DA graphics range.

all: $(PROG).prg $(PROG).d64

$(PROG).prg: $(PROG).s $(CFG)
	$(CA65) $(PROG).s -o $(PROG).o
	$(LD65) -C $(CFG) -o $(PROG).prg $(PROG).o

$(PROG).d64: $(PROG).prg
	$(C1541) -format "tcpser link,tl" d64 $(PROG).d64 >/dev/null
	$(C1541) -attach $(PROG).d64 -write $(PROG).prg $(PROG) >/dev/null
	@echo "built $(PROG).prg and $(PROG).d64"

run: $(PROG).prg
	$(X64) -autostart $(PROG).prg

# Headless render without a 6551 (shows the "NONE" state).
shot: $(PROG).prg
	rm -f shot.png
	$(X64) -default -silent +sound -warp +acia1 -limitcycles 20000000 -exitscreenshot shot.png -autostart $(PROG).prg >/dev/null 2>&1 || true
	@test -f shot.png && echo "wrote shot.png" || echo "no screenshot produced"

# Headless render of a LIVE session: VICE's SwiftLink emulation talks ip232 to
# a tcpser started from ../tcpser (built first with 'make' in the parent dir).
# A caller connects to tcpser's TCP port, tcpser auto-answers (S0=1), raises
# DCD and sends RING/CONNECT, all of which shows up on the C64 screen.
#   make shot-live            -> shot.png
TCPSER   = ../tcpser
IP232    = 25232
TCPPORT  = 16901
shot-live: $(PROG).prg
	@test -x $(TCPSER) || { echo "build tcpser first: (cd .. && make)"; exit 1; }
	rm -f shot.png
	@TCPSER_BLOCKLIST=/tmp/tcpser-link-test.blocklist $(TCPSER) -v $(IP232) -p $(TCPPORT) -l 1 -i S0=1 & echo $$! > .tcpser.pid; sleep 1; \
	( sleep 4; python3 -c "import socket,time; c=socket.create_connection(('127.0.0.1',$(TCPPORT))); time.sleep(6); c.close()" ) & \
	$(X64) -default -silent +sound -warp -acia1 -acia1base 0xDE00 -acia1mode 1 -acia1irq 0 -myaciadev 0 \
	  -rsdev1 127.0.0.1:$(IP232) -rsdev1ip232 -keybuf "a" -limitcycles 90000000 \
	  -exitscreenshot shot.png -autostart $(PROG).prg >/dev/null 2>&1 || true; \
	kill `cat .tcpser.pid` 2>/dev/null; rm -f .tcpser.pid /tmp/tcpser-link-test.blocklist
	@test -f shot.png && echo "wrote shot.png" || echo "no screenshot produced"

# Interactive session against a live tcpser (same wiring as shot-live, no warp,
# window open): press the keys yourself.
run-live: $(PROG).prg
	@test -x $(TCPSER) || { echo "build tcpser first: (cd .. && make)"; exit 1; }
	@TCPSER_BLOCKLIST=/tmp/tcpser-link-test.blocklist $(TCPSER) -v $(IP232) -p $(TCPPORT) -l 4 -i S0=1 & echo $$! > .tcpser.pid; sleep 1; \
	$(X64) -default -acia1 -acia1base 0xDE00 -acia1mode 1 -acia1irq 0 -myaciadev 0 \
	  -rsdev1 127.0.0.1:$(IP232) -rsdev1ip232 -autostart $(PROG).prg; \
	kill `cat .tcpser.pid` 2>/dev/null; rm -f .tcpser.pid /tmp/tcpser-link-test.blocklist

clean:
	rm -f $(PROG).o $(PROG).prg $(PROG).d64 linktest.o linktest.prg linktest.d64 shot.png

.PHONY: all run shot shot-live run-live clean
