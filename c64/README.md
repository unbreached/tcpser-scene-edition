# TCPSER LINK ANALYZER — C64 side

An interactive Commodore 64 program that pairs with **tcpser** to diagnose an
RS232 adapter and its cabling. It runs on the C64 (or in VICE) and reads the
6551 ACIA registers directly — something the PC side of tcpser physically
cannot see — so you can watch the actual DCD / DSR / DTR / RTS handshake bits
and map the wiring against tcpser's control-line monitor.

> Status: **v0.1** — interactive UI, live 6551 register readout, and manual
> DTR/RTS/test-byte controls. The automated tcpser handshake (auto-baud
> negotiation + config) is the next milestone.

## What it does today

- Probes for a 6551 ACIA (SwiftLink / Turbo232 / GLINK232) at `$DE00`.
- **Live register readout**: STATUS (`$DE01`) and COMMAND (`$DE02`), decoded to
  individual bits — DCD, DSR, RX-full, TX-empty, DTR, RTS.
- Interactive keys:
  - `D` — toggle DTR   `R` — toggle RTS   (watch tcpser react to map wiring)
  - `T` — send a `$55` ('U') test byte (alternating bits = auto-baud pattern)
  - `Q` — quit

## Build

Requires **cc65** (`ca65`/`ld65`) and, for the headless preview, **VICE**
(`x64sc`, `c1541`).

```sh
make          # -> tcpser-link.prg and tcpser-link.d64
make run      # build + launch in VICE
make shot     # build + headless VICE render to shot.png
```

Note: the source is assembled **without** `-t c64` on purpose, so string
literals stay raw ASCII (`$41-$5A`) rather than being remapped into the
`$C1-$DA` PETSCII graphics range.

## Run

- **Real C64**: copy `tcpser-link.d64` to an SD2IEC / 1541-Ultimate / real disk
  and `LOAD"*",8,1` then `RUN`, or transfer `tcpser-link.prg` however you like.
- **VICE**: `x64sc -autostart tcpser-link.prg`

In VICE you'll normally see `ADAPTER: NONE` unless you enable SwiftLink/ACIA
emulation (Settings → I/O extensions), because there's no 6551 at `$DE00` by
default. On real hardware with a SwiftLink/Turbo232 cartridge it reads the live
chip.

## Files

| File | Purpose |
|------|---------|
| `tcpser-link.s` | 6502 source (ca65) |
| `c64prg.cfg`    | ld65 config (emits a `$0801` .prg with a BASIC stub) |
| `Makefile`      | build / run / headless-render targets |
| `tcpser-link.prg` / `.d64` | built program (shippable) |
| `shot.png`      | headless VICE render (preview) |

## Roadmap

1. Enable SwiftLink/ACIA + ip232 in VICE and drive a real read/toggle test
   against a running tcpser.
2. Cooperative auto-negotiation: C64 transmits a training pattern → tcpser
   auto-detects baud/framing → asserts DCD → both confirm bidirectional at the
   negotiated rate, then print the known-good `tcpser` command line + C64 init.
