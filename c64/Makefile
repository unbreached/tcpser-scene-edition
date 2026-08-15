# TCPSER LINK ANALYZER — C64 side build
# Requires: cc65 (ca65/ld65) and VICE (x64sc, c1541).
#
#   make            build tcpser-link.prg and tcpser-link.d64
#   make run        build + launch in VICE (x64sc)
#   make shot       build + headless VICE render to shot.png (for CI/preview)

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

# Headless render: run under warp for a fixed number of cycles, screenshot on exit.
shot: $(PROG).prg
	rm -f shot.png
	$(X64) -default -warp -limitcycles 20000000 -exitscreenshot shot.png -autostart $(PROG).prg >/dev/null 2>&1 || true
	@test -f shot.png && echo "wrote shot.png" || echo "no screenshot produced"

clean:
	rm -f $(PROG).o $(PROG).prg $(PROG).d64 linktest.o linktest.prg linktest.d64 shot.png

.PHONY: all run shot clean
