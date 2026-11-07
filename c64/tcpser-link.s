; ===========================================================================
;  TCPSER LINK ANALYZER  (C64 side)  v0.2
;  Interactive RS232 adapter / cable diagnostic that pairs with tcpser.
;
;  - Probes for a 6551 ACIA (SwiftLink / Turbo232 / GLINK232) at $DE00, then
;    $DF00 (the alternate jumper setting). The probe saves and restores the
;    control register, so it leaves the chip exactly as it found it.
;  - LIVE readout of the 6551 STATUS and COMMAND registers, decoded to the
;    state of the RS232 CABLE lines. Two things the old version got wrong:
;    (1) the 6551 status bits are active-LOW (a 0 bit means the line is
;    asserted), and (2) SwiftLink-class cartridges (SwiftLink, Turbo232,
;    GLINK232) feed the RS232 DCD pin into the 6551's DSR input and RS232 DSR
;    into the DCD input, so the chip's receiver is never gated by carrier.
;    So: cable DCD (carrier, driven by tcpser) = status bit 6 == 0, and cable
;    DSR = status bit 5 == 0. Verified against VICE's SwiftLink emulation
;    talking ip232 to tcpser (bit 6 flips when tcpser raises DCD).
;    Parity / framing / overrun errors are latched and shown (framing errors
;    = wrong baud rate).
;  - 256-byte receive ring: the MONITOR page shows the last 8 bytes, the LOG
;    page a 48-byte hex dump and a 144-character text view - so a whole
;    RING / CONNECT / OK exchange from tcpser is readable on the C64.
;  - Four pages (F1/F3/F5/F7 or 1-4): MONITOR, LOG, WIRING, HELP.
;  - Keys: D toggle DTR, R toggle RTS, B cycle baud, I init ACIA (8N1,
;    DTR+RTS on), T send 16 x 'U' (auto-baud pattern), A send "AT"+CR,
;    C clear log/counters, Q quit back to BASIC.
;
;  Assemble WITHOUT -t c64 (keep raw ASCII in string literals):
;     ca65 tcpser-link.s -o tcpser-link.o
;     ld65 -C c64prg.cfg -o tcpser-link.prg tcpser-link.o
;
;  6551 register map (base = $DE00 or $DF00):
;     +0  DATA     read = receive data, write = transmit data
;     +1  STATUS   read; write = programmed reset
;                  b0 parity err  b1 framing err  b2 overrun  b3 RDRF
;                  b4 TDRE  b5 /DCD pin  b6 /DSR pin  b7 IRQ
;                  (on SwiftLink-class carts the CABLE's DCD arrives on b6
;                   and the cable's DSR on b5 - see above)
;     +2  COMMAND  b0 DTR (1=on, enables rx/tx)  b1 rx IRQ disable
;                  b3-2 transmitter control (00 = RTS off, 10 = RTS on, no IRQ)
;                  b4 echo  b7-5 parity mode
;     +3  CONTROL  b3-0 baud  b4 1=baud generator  b6-5 word length (00=8)
;                  b7 stop bits (0=1)
;  SwiftLink / Turbo232 / GLINK232 run the 6551 from a doubled crystal, so
;  every nominal 6551 rate comes out twice as fast; the BAUD line shows the
;  real (doubled) rate.
;
;  Screen layout (40 x 25, character set 1 = uppercase + graphics):
;     0  +--------------------------------------+
;     1  |  title bar (reverse cyan)            |
;     2  +--------------------------------------+
;     3  |  ADAPTER ...                         |
;     4  |  BAUD ...                            |
;     5  +--------------------------------------+
;     6  |  F1 MONITOR  F3 LOG  F5 WIRING  F7 HELP   (tab bar)
;     7  +--------------------------------------+
;     8  |  page area, 12 rows (8..19)          |
;    19  |                                      |
;    20  +--------------------------------------+
;    21  |  status message (yellow)             |
;    22  |  key hint line                       |
;    23  +--------------------------------------+
;    24     footer (no frame: printing col 39 on the last row would scroll)
; ===========================================================================

; ---- KERNAL -----
CHROUT  = $FFD2
GETIN   = $FFE4
PLOT    = $FFF0        ; carry clear: set cursor row=X col=Y
JIFFY   = $A2          ; low byte of the 1/60 s jiffy clock

; ---- VIC colors -----
VBORDER = $D020
VBG     = $D021

; ---- zero page scratch -----
str     = $FB          ; string pointer (2)
acia    = $FD          ; ACIA base pointer (2)
tmp     = $02          ; scratch (officially unused byte)

; ---- 6551 register offsets (use with (acia),y) -----
R_DATA  = 0
R_STAT  = 1
R_CMD   = 2
R_CTRL  = 3

CMD_INIT  = $0B        ; DTR on, rx IRQ disabled, RTS on (no tx IRQ), no parity
CTRL_BASE = $10        ; baud generator, 8 data bits, 1 stop bit (+ baud nibble)
NBAUD     = 7
DEF_BAUD  = 2          ; index into baud tables: 2400

; ---- pages -----
PG_MON  = 0
PG_LOG  = 1
PG_WIRE = 2
PG_HELP = 3
NPAGES  = 4

; ---- screen rows -----
ROW_ADAPTER = 3
ROW_BAUD    = 4
ROW_TABS    = 6
PAGE_TOP    = 8
PAGE_ROWS   = 12
ROW_MSG     = 21
ROW_HINT    = 22
ROW_FOOT    = 24
; monitor page
SEC_REGS    = 8
ROW_REGS    = 9
ROW_LINES   = 10
ROW_FLAGS   = 11
SEC_LINK    = 12
ROW_HEX     = 13
ROW_TEXT    = 14
ROW_COUNT   = 15
SEC_KEYS    = 16
ROW_KEYS1   = 17
ROW_KEYS2   = 18
; log page
SEC_DUMP    = 8
ROW_DUMP    = 9         ; 6 rows: 9..14
SEC_TXT     = 15
ROW_TXT     = 16        ; 4 rows: 16..19
; help page
SEC_HKEYS   = 8
ROW_HKEYS   = 9         ; 4 rows
SEC_HOWTO   = 13
ROW_HOWTO   = 14        ; 6 rows

; ---- PETSCII codes -----
RVON    = $12
RVOFF   = $92
WHITE   = $05
CYAN    = $9F
YELLOW  = $9E
LGREEN  = $99
RED     = $1C
GREY    = $98          ; grey 2 (medium) - "off" states, inactive tabs
LGREY   = $9B          ; grey 3 (light)  - hints
LBLUE   = $9A
CLS     = $93
K_F1    = $85
K_F3    = $86
K_F5    = $87
K_F7    = $88

; PETSCII box-drawing glyphs (character set 1)
G_HOR   = $C0          ; horizontal line   (SHIFT-*)
G_VER   = $DD          ; vertical line     (SHIFT--)
G_TL    = $B0          ; top-left corner   (C=-A)
G_TR    = $AE          ; top-right corner  (C=-S)
G_BL    = $AD          ; bottom-left       (C=-Z)
G_BR    = $BD          ; bottom-right      (C=-X)
G_LT    = $AB          ; left tee  |-      (C=-E)
G_RT    = $B3          ; right tee -|      (C=-W)
G_SHADE = $A6          ; checkerboard shading (C=-+)

.segment "HEADER"
        .word $0801
.segment "CODE"
        .word bstub_end, 10
        .byte $9e, "2061", $00
bstub_end:
        .word 0

; =========================== entry ========================================
start:
        lda #0
        sta VBORDER            ; black border
        sta VBG                ; black background
        lda #CLS
        jsr CHROUT
        ; stay in character set 1 (uppercase + graphics): the raw ASCII in
        ; our strings prints as uppercase there, and the box glyphs exist

        lda #DEF_BAUD
        sta baud_idx
        lda #PG_MON
        sta page
        jsr reset_counters
        jsr detect_acia        ; sets have_acia, acia ptr
        jsr init_acia          ; 8N1 at the default baud, DTR+RTS on
        jsr draw_frame         ; static chrome
        jsr draw_page          ; current page's static part
        jsr refresh
        lda #<m_hello
        ldy #>m_hello
        jsr msg
        lda JIFFY
        sta last_tick

mainloop:
        jsr poll_rx            ; every iteration: never miss a byte
        jsr handle_key
        ; refresh the display ~10x per second (6 jiffies)
        lda JIFFY
        sec
        sbc last_tick
        cmp #6
        bcc mainloop
        lda JIFFY
        sta last_tick
        jsr refresh
        jmp mainloop

; ===================== adapter detection ==================================
; Try $DE00 then $DF00. probe_here: write two patterns to the control
; register and read them back; a real 6551 latches them, open bus does not.
; The original control value is restored afterwards (the old probe left the
; chip reprogrammed to 19200 baud).
detect_acia:
        lda #0
        sta have_acia
        sta acia
        lda #$de
        sta acia+1
        jsr probe_here
        bne @found
        lda #$df
        sta acia+1
        jsr probe_here
        bne @found
        lda #$de               ; nothing: point at $DE00 for the display
        sta acia+1
        rts
@found: lda #1
        sta have_acia
        rts

; Z=0 (A=1) if a 6551 answers at (acia)
probe_here:
        ldy #R_CTRL
        lda (acia),y
        sta tmp                ; remember original control register
        lda #$16
        sta (acia),y
        lda (acia),y
        cmp #$16
        bne @no
        lda #$1e
        sta (acia),y
        lda (acia),y
        cmp #$1e
        bne @no
        lda tmp
        sta (acia),y           ; restore
        lda #1
        rts
@no:    lda tmp
        sta (acia),y           ; restore even on failure (harmless on open bus)
        lda #0
        rts

; ===================== ACIA setup =========================================
; Programmed reset, then 8N1 at baud_idx, DTR + RTS asserted, IRQs off.
init_acia:
        lda have_acia
        beq @done
        ldy #R_STAT
        sta (acia),y           ; any write to STATUS = programmed reset
        jsr apply_baud
        ldy #R_CMD
        lda #CMD_INIT
        sta (acia),y
@done:  rts

apply_baud:
        lda have_acia
        beq @done
        ldx baud_idx
        lda baud_nib,x
        ora #CTRL_BASE
        ldy #R_CTRL
        sta (acia),y
@done:  rts

; ===================== receive polling ====================================
; Read STATUS; latch error bits; if RDRF, pull the byte into the 256-byte
; ring. Clobbers A, X, Y (putc saves them around the call).
poll_rx:
        lda have_acia
        beq @done
        ldy #R_STAT
        lda (acia),y
        sta last_status
        and #$07
        beq @noerr
        ora err_latched
        sta err_latched
        lda last_status
        and #$02               ; framing error -> counter
        beq @noerr
        inc frm_count
        bne @noerr
        inc frm_count+1
@noerr: lda last_status
        and #$08               ; RDRF
        beq @done
        ldy #R_DATA
        lda (acia),y
        ldx rxhead
        sta rxbuf,x
        inc rxhead             ; 8-bit index wraps the 256-byte ring for free
        inc rx_count
        bne @done
        inc rx_count+1
@done:  rts

; ===================== transmit ===========================================
; Send byte in A. Waits for TDRE with a timeout so a wedged chip cannot hang
; the program. Sets tx_timeout=1 if the byte could not be sent.
send_byte:
        sta tmp
        ldx #0
@wait:  ldy #R_STAT
        lda (acia),y
        and #$10               ; TDRE
        bne @go
        ldy #0
@d:     dey
        bne @d
        dex
        bne @wait
        lda #1
        sta tx_timeout
        rts
@go:    lda tmp
        ldy #R_DATA
        sta (acia),y
        inc tx_count
        bne @ok
        inc tx_count+1
@ok:    rts

; ===================== static chrome ======================================
draw_frame:
        lda #CYAN
        jsr putc
        ldx #0
        jsr row_top
        ldx #2
        jsr row_sep
        ldx #5
        jsr row_sep
        ldx #7
        jsr row_sep
        ldx #20
        jsr row_sep
        ldx #23
        jsr row_bottom
        ; vertical bars on every content row 1..22
        ldx #1
@bars:  stx tmp
        ldy #0
        jsr gotoxy
        lda #G_VER
        jsr putc
        ldx tmp
        ldy #39
        jsr gotoxy
        lda #G_VER
        jsr putc
        ldx tmp
        inx
        cpx #23
        bne @bars

        ; --- title bar: reverse cyan across the 38 inner columns ---
        ldx #1
        ldy #1
        jsr gotoxy
        lda #RVON
        jsr putc
        lda #CYAN
        jsr putc
        lda #<t_title
        ldy #>t_title
        jsr prstr
        lda #RVOFF
        jsr putc

        ; --- adapter line ---
        ldx #ROW_ADAPTER
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<t_adapter
        ldy #>t_adapter
        jsr prstr
        lda have_acia
        bne @yes
        lda #RED
        jsr putc
        lda #<t_none
        ldy #>t_none
        jsr prstr
        jmp @baud
@yes:   lda #LGREEN
        jsr putc
        lda #<t_found
        ldy #>t_found
        jsr prstr
        jsr prbase             ; " $DE00"

@baud:  ldx #ROW_BAUD
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<t_baud
        ldy #>t_baud
        jsr prstr              ; "BAUD    " (value drawn by refresh)
        ldx #ROW_BAUD
        ldy #16
        jsr gotoxy
        lda #LGREY
        jsr putc
        lda #<t_rate
        ldy #>t_rate
        jsr prstr

        ; --- key hint line (row 22) ---
        ldx #ROW_HINT
        ldy #2
        jsr gotoxy
        lda #LGREY
        jsr putc
        lda #<t_hintline
        ldy #>t_hintline
        jsr prstr

        ; --- footer (row 24, never touch column 39) ---
        ldx #ROW_FOOT
        ldy #1
        jsr gotoxy
        lda #LBLUE
        jsr putc
        lda #<t_footer
        ldy #>t_footer
        jsr prstr
        jmp draw_tabs

; tab bar (row 6): active page in reverse cyan, others grey
draw_tabs:
        ldx #ROW_TABS
        ldy #1
        jsr gotoxy
        ldx #0
@t:     stx tmp
        cpx page
        bne @inact
        lda #RVON
        jsr putc
        lda #CYAN
        jsr putc
        jmp @name
@inact: lda #GREY
        jsr putc
@name:  ldx tmp
        lda tab_lo,x
        ldy tab_hi,x
        jsr prstr
        lda #RVOFF
        jsr putc
        ldx tmp
        inx
        cpx #NPAGES
        bne @t
        rts

; ---- frame primitives (current colour is used; callers set CYAN) --------
row_top:
        ldy #0
        jsr gotoxy
        lda #G_TL
        jsr putc
        jsr hor38
        lda #G_TR
        jmp putc
row_bottom:
        ldy #0
        jsr gotoxy
        lda #G_BL
        jsr putc
        jsr hor38
        lda #G_BR
        jmp putc
row_sep:
        ldy #0
        jsr gotoxy
        lda #G_LT
        jsr putc
        jsr hor38
        lda #G_RT
        jmp putc
; X = row, A/Y = title string. Separator with " TITLE " inlaid in yellow:
;   |-- TITLE -----...-----|
row_section:
        sta str
        sty str+1
        ldy #0
        jsr gotoxy
        lda #CYAN
        jsr putc
        lda #G_LT
        jsr putc
        lda #G_HOR
        jsr putc
        lda #YELLOW
        jsr putc
        lda str
        ldy str+1
        jsr prstr              ; Y = title length on return
        tya
        sta tmp
        lda #CYAN
        jsr putc
        lda #37                ; 38 inner - 1 leading line
        sec
        sbc tmp
        tax
@fill:  lda #G_HOR
        jsr putc
        dex
        bne @fill
        lda #G_RT
        jmp putc
hor38:  ldx #38
@l:     lda #G_HOR
        jsr putc
        dex
        bne @l
        rts

; ===================== pages ==============================================
; Blank the page area (rows 8..19) including the side bars, then draw the
; static part of the current page.
draw_page:
        jsr draw_tabs
        lda #CYAN
        jsr putc
        ldx #PAGE_TOP
@row:   stx tmp
        ldy #0
        jsr gotoxy
        lda #G_VER
        jsr putc
        ldx #38
        lda #' '
@sp:    jsr putc
        dex
        bne @sp
        lda #G_VER
        jsr putc
        ldx tmp
        inx
        cpx #PAGE_TOP+PAGE_ROWS
        bne @row
        lda page
        asl a
        tax
        lda page_draw+1,x
        pha
        lda page_draw,x
        pha
        rts
page_draw: .word page_mon_draw-1, page_log_draw-1, page_wire_draw-1, page_help_draw-1

; ---- MONITOR page: static labels ------------------------------------------
page_mon_draw:
        ldx #SEC_REGS
        lda #<t_regs
        ldy #>t_regs
        jsr row_section
        ldx #SEC_LINK
        lda #<t_link
        ldy #>t_link
        jsr row_section
        ldx #SEC_KEYS
        lda #<t_keys
        ldy #>t_keys
        jsr row_section

        lda have_acia
        bne @labels
        ldx #ROW_LINES
        ldy #2
        jsr gotoxy
        lda #RED
        jsr putc
        lda #<t_nolive
        ldy #>t_nolive
        jsr prstr
        jmp @link

@labels:
        ; "STATUS $xx01=" col 2, "COMMAND $xx02=" col 19
        ldx #ROW_REGS
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<t_status
        ldy #>t_status
        jsr prstr
        lda acia+1
        jsr prhex
        lda #<t_01
        ldy #>t_01
        jsr prstr
        ldx #ROW_REGS
        ldy #19
        jsr gotoxy
        lda #<t_cmd
        ldy #>t_cmd
        jsr prstr
        lda acia+1
        jsr prhex
        lda #<t_02
        ldy #>t_02
        jsr prstr

@link:  ldx #ROW_HEX
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<t_lastrx
        ldy #>t_lastrx
        jsr prstr
        ldx #ROW_TEXT
        ldy #2
        jsr gotoxy
        lda #<t_astext
        ldy #>t_astext
        jsr prstr

        ldx #ROW_KEYS1
        ldy #2
        jsr gotoxy
        lda #<t_menu1
        ldy #>t_menu1
        jsr prmenu
        ldx #ROW_KEYS2
        ldy #2
        jsr gotoxy
        lda #<t_menu2
        ldy #>t_menu2
        jmp prmenu

; ---- LOG page: static labels ---------------------------------------------
page_log_draw:
        ldx #SEC_DUMP
        lda #<t_dump
        ldy #>t_dump
        jsr row_section
        ldx #SEC_TXT
        lda #<t_txt
        ldy #>t_txt
        jmp row_section

; ---- WIRING page: all static -------------------------------------------------
page_wire_draw:
        ldx #PAGE_TOP
        lda #<t_wire0
        ldy #>t_wire0
        jsr row_section
        ldx #PAGE_TOP+1
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<w_head
        ldy #>w_head
        jsr prstr
        ; seven cable lines: "   TXD " + 10 line glyphs + " RXD" (+ note)
        ldx #0
@w:     stx tmp2
        txa
        clc
        adc #PAGE_TOP+2
        tax
        ldy #2
        jsr gotoxy
        lda #LGREEN
        jsr putc
        ldx tmp2
        lda wire_lo,x
        ldy wire_hi,x
        jsr prstr
        lda #CYAN
        jsr putc
        ldx #10
@ln:    lda #G_HOR
        jsr putc
        dex
        bne @ln
        lda #LGREEN
        jsr putc
        ldx tmp2
        lda wire2_lo,x
        ldy wire2_hi,x
        jsr prstr
        ldx tmp2
        inx
        cpx #7
        bne @w
        ldx #PAGE_TOP+9
        lda #<t_swap
        ldy #>t_swap
        jsr row_section
        ldx #PAGE_TOP+10
        ldy #2
        jsr gotoxy
        lda #LGREY
        jsr putc
        lda #<w_swap1
        ldy #>w_swap1
        jsr prstr
        ldx #PAGE_TOP+11
        ldy #2
        jsr gotoxy
        lda #<w_swap2
        ldy #>w_swap2
        jmp prstr

; ---- HELP page: all static --------------------------------------------------
page_help_draw:
        ldx #SEC_HKEYS
        lda #<t_keys
        ldy #>t_keys
        jsr row_section
        ldx #0
@k:     stx tmp2
        txa
        clc
        adc #ROW_HKEYS
        tax
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        ldx tmp2
        lda hk_lo,x
        ldy hk_hi,x
        jsr prmenu
        ldx tmp2
        inx
        cpx #4
        bne @k
        ldx #SEC_HOWTO
        lda #<t_howto
        ldy #>t_howto
        jsr row_section
        ldx #0
@h:     stx tmp2
        txa
        clc
        adc #ROW_HOWTO
        tax
        ldy #2
        jsr gotoxy
        lda #LGREY
        jsr putc
        ldx tmp2
        lda hh_lo,x
        ldy hh_hi,x
        jsr prstr
        ldx tmp2
        inx
        cpx #6
        bne @h
        rts

; print a key menu string: '[' switches to reverse cyan, ']' back to white.
prmenu:
        sta str
        sty str+1
        ldy #0
@l:     lda (str),y
        beq @e
        cmp #'['
        beq @on
        cmp #']'
        beq @off
        jsr putc
@n:     iny
        bne @l
@e:     rts
@on:    lda #RVON
        jsr putc
        lda #CYAN
        jsr putc
        jmp @n
@off:   lda #RVOFF
        jsr putc
        lda #WHITE
        jsr putc
        jmp @n

; print " $" + base_hi + "00"
prbase: lda #' '
        jsr putc
        lda #'$'
        jsr putc
        lda acia+1
        jsr prhex
        lda #'0'
        jsr putc
        lda #'0'
        jmp putc

; ===================== live refresh =======================================
refresh:
        ; BAUD value (row 4, col 10, 5 chars right-aligned) - always
        ldx #ROW_BAUD
        ldy #10
        jsr gotoxy
        lda #CYAN
        jsr putc
        ldx baud_idx
        lda baud_lo,x
        ldy baud_hi,x
        jsr prstr
        lda page
        beq refresh_mon
        cmp #PG_LOG
        bne :+
        jmp refresh_log        ; too far for a branch
:       rts

; ---- MONITOR page dynamic part --------------------------------------------
refresh_mon:
        lda have_acia
        bne @live
        rts

@live:  ; STATUS hex (col 15) and COMMAND hex (col 33)
        ldx #ROW_REGS
        ldy #15
        jsr gotoxy
        lda #CYAN
        jsr putc
        lda #'$'
        jsr putc
        lda last_status
        jsr prhex
        ldx #ROW_REGS
        ldy #33
        jsr gotoxy
        lda #CYAN
        jsr putc
        lda #'$'
        jsr putc
        ldy #R_CMD
        lda (acia),y
        sta last_cmd
        jsr prhex

        ; DCD / DSR / DTR / RTS
        ldx #ROW_LINES
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<l_dcd
        ldy #>l_dcd
        jsr prstr
        lda last_status
        and #$40               ; cable DCD = 6551 "DSR" pin on SwiftLink; 0 = carrier
        bne @nodcd
        lda #<s_carrier
        ldy #>s_carrier
        jsr pr_on
        jmp @dsr
@nodcd: lda #<s_nocar
        ldy #>s_nocar
        jsr pr_off
@dsr:   lda #<l_dsr
        ldy #>l_dsr
        jsr prstr
        lda last_status
        and #$20               ; cable DSR = 6551 "DCD" pin on SwiftLink; 0 = ready
        bne @nodsr
        lda #<s_ready
        ldy #>s_ready
        jsr pr_on
        jmp @dtr
@nodsr: lda #<s_off5
        ldy #>s_off5
        jsr pr_off
@dtr:   lda #<l_dtr
        ldy #>l_dtr
        jsr prstr
        lda last_cmd
        and #$01
        beq @nodtr
        lda #<s_on3
        ldy #>s_on3
        jsr pr_on
        jmp @rts
@nodtr: lda #<s_off3
        ldy #>s_off3
        jsr pr_off
@rts:   lda #<l_rts
        ldy #>l_rts
        jsr prstr
        lda last_cmd
        and #$0c               ; 00 = RTS off, anything else = RTS asserted
        beq @norts
        lda #<s_on3
        ldy #>s_on3
        jsr pr_on
        jmp @flags
@norts: lda #<s_off3
        ldy #>s_off3
        jsr pr_off

@flags: ; RX / TX / latched errors
        ldx #ROW_FLAGS
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<l_rx
        ldy #>l_rx
        jsr prstr
        lda last_status
        and #$08
        beq @rxidle
        lda #<s_data
        ldy #>s_data
        jsr pr_on
        jmp @tx
@rxidle:lda #<s_idle
        ldy #>s_idle
        jsr pr_off
@tx:    lda #<l_tx
        ldy #>l_tx
        jsr prstr
        lda last_status
        and #$10
        beq @txbusy
        lda #<s_ready
        ldy #>s_ready
        jsr pr_on
        jmp @err
@txbusy:lda #<s_busy
        ldy #>s_busy
        jsr pr_off
@err:   lda #<l_err
        ldy #>l_err
        jsr prstr
        lda err_latched
        and #$01
        ldx #<s_par
        ldy #>s_par
        jsr pr_flag
        lda err_latched
        and #$02
        ldx #<s_frm
        ldy #>s_frm
        jsr pr_flag
        lda err_latched
        and #$04
        ldx #<s_ovr
        ldy #>s_ovr
        jsr pr_flag

        ; last 8 received bytes as hex (oldest first), col 11
        ldx #ROW_HEX
        ldy #11
        jsr gotoxy
        lda #CYAN
        jsr putc
        lda rxhead
        sec
        sbc #8
        sta ridx
        ldx #8
        jsr dump_hex           ; 8 bytes "xx "
        ; same bytes as text, col 11
        ldx #ROW_TEXT
        ldy #11
        jsr gotoxy
        lda #LGREEN
        jsr putc
        lda rxhead
        sec
        sbc #8
        sta ridx
        ldx #8
        jsr dump_txt

        ; counters
        ldx #ROW_COUNT
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr putc
        lda #<l_txc
        ldy #>l_txc
        jsr prstr
        lda #CYAN
        jsr putc
        lda tx_count
        ldx tx_count+1
        jsr prdec16
        lda #WHITE
        jsr putc
        lda #<l_rxc
        ldy #>l_rxc
        jsr prstr
        lda #CYAN
        jsr putc
        lda rx_count
        ldx rx_count+1
        jsr prdec16
        lda #WHITE
        jsr putc
        lda #<l_frc
        ldy #>l_frc
        jsr prstr
        lda frm_count
        ora frm_count+1
        beq @frok
        lda #RED
        jsr putc
        jmp @frp
@frok:  lda #CYAN
        jsr putc
@frp:   lda frm_count
        ldx frm_count+1
        jmp prdec16

; ---- LOG page dynamic part -------------------------------------------------
; 6 rows x 8 bytes hex + text (last 48 bytes), then 4 rows x 36 chars text
; (last 144 bytes). Oldest first, newest bottom-right.
refresh_log:
        lda have_acia
        bne :+
        rts
:       lda rxhead
        sec
        sbc #48
        sta ridx
        ldx #0
@row:   stx tmp2
        txa
        clc
        adc #ROW_DUMP
        tax
        ldy #2
        jsr gotoxy
        lda #CYAN
        jsr putc
        lda ridx
        pha                    ; remember row start for the text column
        ldx #8
        jsr dump_hex           ; advances ridx by 8
        lda #' '
        jsr putc
        lda #LGREEN
        jsr putc
        pla
        pha
        sta ridx
        ldx #8
        jsr dump_txt           ; advances ridx by 8 again
        pla
        clc
        adc #8
        sta ridx
        ldx tmp2
        inx
        cpx #6
        bne @row

        lda rxhead
        sec
        sbc #144
        sta ridx
        ldx #0
@trow:  stx tmp2
        txa
        clc
        adc #ROW_TXT
        tax
        ldy #2
        jsr gotoxy
        lda #LGREEN
        jsr putc
        ldx #36
        jsr dump_txt
        ldx tmp2
        inx
        cpx #4
        bne @trow
        rts

; print X bytes from rxbuf[ridx] as "xx " each; advances ridx
dump_hex:
        stx dcount
@l:     ldx ridx
        lda rxbuf,x
        inc ridx
        jsr prhex
        lda #' '
        jsr putc
        dec dcount
        bne @l
        rts
; print X bytes from rxbuf[ridx] as text; advances ridx
dump_txt:
        stx dcount
@l:     ldx ridx
        lda rxbuf,x
        inc ridx
        jsr prchar
        dec dcount
        bne @l
        rts

; print string (A/Y) in light green
pr_on:  pha
        lda #LGREEN
        jsr putc
        pla
        jmp prstr
; print string (A/Y) in grey
pr_off: pha
        lda #GREY
        jsr putc
        pla
        jmp prstr
; A = flag (0/non-0), X/Y = string: red if set, grey if clear
pr_flag:
        cmp #0
        beq @clr
        lda #RED
        jsr putc
        jmp @p
@clr:   lda #GREY
        jsr putc
@p:     txa
        jmp prstr

; ===================== keyboard ===========================================
handle_key:
        jsr GETIN
        beq @done
        ; function keys select pages (before the $7f mask)
        cmp #K_F1
        bcc @letter
        cmp #K_F7+1
        bcs @letter
        sec
        sbc #K_F1
        jmp set_page
@letter:
        and #$7f               ; accept shifted letters too
        cmp #'1'
        bcc @scan0
        cmp #'4'+1
        bcs @scan0
        sec
        sbc #'1'               ; '1'..'4' also select pages
        jmp set_page
@scan0: ldx #0
@scan:  cmp key_tab,x
        beq @hit
        inx
        cpx #NKEYS
        bne @scan
@done:  rts
@hit:   txa
        asl a
        tax
        lda key_jmp+1,x
        pha
        lda key_jmp,x
        pha
        rts                    ; "jump" to the handler via the stack

NKEYS = 8
key_tab: .byte "DRBICTAQ"
; addresses minus 1, because RTS adds 1
key_jmp: .word key_dtr-1, key_rts-1, key_baud-1, key_init-1
         .word key_clear-1, key_test-1, key_at-1, key_quit-1

; A = page number
set_page:
        cmp page
        beq @same
        sta page
        jsr draw_page
        jmp refresh
@same:  rts

key_dtr:
        lda have_acia
        bne :+
        jmp noacia
:       ldy #R_CMD
        lda (acia),y
        eor #$01               ; flip DTR
        sta (acia),y
        and #$01
        beq @off
        lda #<m_dtr_on
        ldy #>m_dtr_on
        jmp msg
@off:   lda #<m_dtr_off
        ldy #>m_dtr_off
        jmp msg

key_rts:
        lda have_acia
        bne :+
        jmp noacia
:       ldy #R_CMD
        lda (acia),y
        and #$0c
        beq @assert
        lda (acia),y
        and #$f3               ; transmitter control 00: RTS off
        sta (acia),y
        lda #<m_rts_off
        ldy #>m_rts_off
        jmp msg
@assert:lda (acia),y
        and #$f3
        ora #$08               ; 10: RTS on, no tx IRQ (never 11 = BREAK)
        sta (acia),y
        lda #<m_rts_on
        ldy #>m_rts_on
        jmp msg

key_baud:
        ldx baud_idx
        inx
        cpx #NBAUD
        bne @ok
        ldx #0
@ok:    stx baud_idx
        jsr apply_baud
        lda #<m_baud
        ldy #>m_baud
        jmp msg

key_init:
        lda have_acia
        bne :+
        jmp noacia
:       jsr init_acia
        lda #<m_init
        ldy #>m_init
        jmp msg

key_clear:
        jsr reset_counters
        jsr draw_page          ; wipes stale hex/text instantly
        jsr refresh
        lda #<m_clear
        ldy #>m_clear
        jmp msg

key_test:
        lda have_acia
        bne :+
        jmp noacia
:       lda #0
        sta tx_timeout
        ldx #16
@l:     txa
        pha
        lda #$55               ; 'U' = 01010101, the auto-baud pattern
        jsr send_byte
        pla
        tax
        dex
        bne @l
        lda tx_timeout
        beq :+
        jmp txfail
:       lda #<m_test
        ldy #>m_test
        jmp msg

key_at:
        lda have_acia
        bne :+
        jmp noacia
:       lda #0
        sta tx_timeout
        lda #'A'
        jsr send_byte
        lda #'T'
        jsr send_byte
        lda #$0d
        jsr send_byte
        lda tx_timeout
        beq :+
        jmp txfail
:       lda #<m_at
        ldy #>m_at
        jmp msg

txfail: lda #<m_txfail
        ldy #>m_txfail
        jmp msg

noacia: lda #<m_noacia
        ldy #>m_noacia
        jmp msg

key_quit:
        lda #14                ; restore the default C64 look
        sta VBORDER
        lda #6
        sta VBG
        lda #CLS
        jsr CHROUT
        lda #LBLUE
        jsr CHROUT
        lda #<m_bye
        ldy #>m_bye
        jsr prstr
        ; We arrived here through handle_key's RTS-dispatch, so the stack now
        ; holds only mainloop's return address. Drop it and return to the
        ; BASIC stub instead.
        pla
        pla
        rts

; ===================== status line ========================================
; msg: print a message on ROW_MSG (yellow), cols 2..37. A=lo, Y=hi.
msg:
        sta msgptr
        sty msgptr+1
        ldx #ROW_MSG
        ldy #1
        jsr gotoxy
        ldx #38                ; blank the inner width
        lda #' '
@clr:   jsr putc
        dex
        bne @clr
        ldx #ROW_MSG
        ldy #2
        jsr gotoxy
        lda #YELLOW
        jsr putc
        lda msgptr
        ldy msgptr+1
        jmp prstr

reset_counters:
        lda #0
        sta tx_count
        sta tx_count+1
        sta rx_count
        sta rx_count+1
        sta frm_count
        sta frm_count+1
        sta err_latched
        sta rxhead
        sta tx_timeout
        ldx #0
@z:     sta rxbuf,x
        inx
        bne @z
        rts

; ===================== helpers ============================================
; putc: CHROUT that keeps the receiver serviced. CHROUT takes ~100 us and a
; full refresh prints ~250 characters; at 2400 baud a byte lands every 4 ms,
; so polling only once per main-loop pass lost bytes (RX 6 of 9 in testing).
; Preserves A, X, Y.
putc:
        sta pc_a
        stx pc_x
        sty pc_y
        jsr CHROUT
        jsr poll_rx
        ldy pc_y
        ldx pc_x
        lda pc_a
        rts

; print null-terminated string: A=lo, Y=hi. Returns Y = length.
prstr:
        sta str
        sty str+1
        ldy #0
@l:     lda (str),y
        beq @e
        jsr putc
        iny
        bne @l
@e:     rts

; cursor to row=X, col=Y
gotoxy:
        clc
        jsr PLOT
        rts

; print A as two hex digits
prhex:
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        jsr prnyb
        pla
        and #$0f
        jsr prnyb
        rts
prnyb:
        and #$0f
        ora #$30
        cmp #$3a
        bcc @o
        adc #$06
@o:     jmp putc

; print 16-bit value A (lo) / X (hi) as five decimal digits
prdec16:
        sta d16
        stx d16+1
        ldx #0                 ; index into the power-of-ten table
@digit: lda #0
        sta digit
@sub:   lda d16
        sec
        sbc pow10_lo,x
        tay
        lda d16+1
        sbc pow10_hi,x
        bcc @out               ; went negative: digit done
        sta d16+1
        sty d16
        inc digit
        jmp @sub
@out:   lda digit
        ora #$30
        jsr putc
        inx
        cpx #4
        bne @digit
        lda d16                ; what is left is the units digit
        ora #$30
        jmp putc
pow10_lo: .byte <10000, <1000, <100, <10
pow10_hi: .byte >10000, >1000, >100, >10

; print a received byte as text: 7-bit ASCII, lowercase folded to uppercase,
; anything unprintable as '.'
prchar:
        and #$7f
        cmp #$20
        bcc @dot
        cmp #$7f
        beq @dot
        cmp #$61
        bcc @ok
        cmp #$7b
        bcs @ok
        and #$5f               ; a-z -> A-Z (PETSCII set 1 shows uppercase)
@ok:    jmp putc
@dot:   lda #'.'
        jmp putc

; ===================== data ===============================================
have_acia:   .byte 0
page:        .byte PG_MON
baud_idx:    .byte DEF_BAUD
last_status: .byte 0
last_cmd:    .byte 0
last_tick:   .byte 0
err_latched: .byte 0
tx_count:    .word 0
rx_count:    .word 0
frm_count:   .word 0
tx_timeout:  .byte 0
rxhead:      .byte 0
ridx:        .byte 0
dcount:      .byte 0
digit:       .byte 0
tmp2:        .byte 0
d16:         .word 0
msgptr:      .word 0
pc_a:        .byte 0
pc_x:        .byte 0
pc_y:        .byte 0
rxbuf:       .res 256, 0

; 6551 control-register baud nibbles and the REAL (doubled) rate they give
; on SwiftLink / Turbo232 / GLINK232 hardware.
baud_nib:    .byte $05, $07, $08, $0a, $0c, $0e, $0f
baud_lo:     .byte <b300, <b1200, <b2400, <b4800, <b9600, <b19200, <b38400
baud_hi:     .byte >b300, >b1200, >b2400, >b4800, >b9600, >b19200, >b38400
b300:        .byte "  300", 0
b1200:       .byte " 1200", 0
b2400:       .byte " 2400", 0
b4800:       .byte " 4800", 0
b9600:       .byte " 9600", 0
b19200:      .byte "19200", 0
b38400:      .byte "38400", 0

; tab names carry their own padding and are printed back to back from
; column 1: 12 + 8 + 9 + 9 = 38 = exactly the inner width.
tab_lo:   .byte <tb_mon, <tb_log, <tb_wire, <tb_help
tab_hi:   .byte >tb_mon, >tb_log, >tb_wire, >tb_help
tb_mon:   .byte " F1 MONITOR ", 0      ; 12
tb_log:   .byte " F3 LOG ", 0          ;  8
tb_wire:  .byte " F5 WIRE ", 0         ;  9
tb_help:  .byte " F7 HELP ", 0         ;  9

; title bar: exactly 38 characters (inner width), symmetric shading
t_title:  .byte G_SHADE, G_SHADE, G_SHADE, G_SHADE, G_SHADE, " "
          .byte "TCPSER LINK ANALYZER  V0.2"
          .byte " ", G_SHADE, G_SHADE, G_SHADE, G_SHADE, G_SHADE, 0
; section titles (inlaid in the separators)
t_regs:   .byte " 6551 REGISTERS ", 0
t_link:   .byte " LINK TEST ", 0
t_keys:   .byte " KEYS ", 0
t_howto:  .byte " HOW TO ", 0
t_dump:   .byte " RX HEX DUMP - LAST 48 BYTES ", 0
t_txt:    .byte " RX AS TEXT - LAST 144 BYTES ", 0
t_wire0:  .byte " CABLE: PC USB-SERIAL TO C64 CART ", 0
t_swap:   .byte " SWIFTLINK PIN SWAP ", 0

;                     123456789012345678901234567890123456  (36 usable)
t_adapter:.byte "ADAPTER ", 0
t_found:  .byte "6551 ACIA AT", 0
t_none:   .byte "NONE AT $DE00 OR $DF00", 0
t_nolive: .byte "NO 6551 FOUND: CHECK CART + JUMPERS", 0
t_baud:   .byte "BAUD    ", 0
t_rate:   .byte "8N1  (SWIFTLINK RATE)", 0
t_status: .byte "STATUS $", 0
t_01:     .byte "01=", 0
t_cmd:    .byte "COMMAND $", 0
t_02:     .byte "02=", 0
t_lastrx: .byte "LAST RX  ", 0
t_astext: .byte "AS TEXT  ", 0
t_menu1:  .byte "[D] DTR  [R] RTS  [B] BAUD  [I] INIT", 0
t_menu2:  .byte "[T] SEND U  [A] SEND AT  [C] CLR  [Q] QUIT", 0
t_hintline:.byte "F1-F7 OR 1-4 PAGES   Q QUIT TO BASIC", 0
t_footer: .byte "DCD/DSR = CABLE STATE (6551 BITS 6/5)", 0

; wiring page: left signal, 14 line glyphs drawn by code, right signal + note
wire_lo:  .byte <w1a, <w2a, <w3a, <w4a, <w5a, <w6a, <w7a
wire_hi:  .byte >w1a, >w2a, >w3a, >w4a, >w5a, >w6a, >w7a
wire2_lo: .byte <w1b, <w2b, <w3b, <w4b, <w5b, <w6b, <w7b
wire2_hi: .byte >w1b, >w2b, >w3b, >w4b, >w5b, >w6b, >w7b
w_head:   .byte "PC (USB-SERIAL)   C64 CARTRIDGE", 0
w1a:      .byte "   TXD ", 0
w1b:      .byte " RXD", 0
w2a:      .byte "   RXD ", 0
w2b:      .byte " TXD", 0
w3a:      .byte "   RTS ", 0
w3b:      .byte " CTS", 0
w4a:      .byte "   CTS ", 0
w4b:      .byte " RTS", 0
w5a:      .byte "   DTR ", 0
w5b:      .byte " DCD  <- CARRIER", 0
w6a:      .byte "   DCD ", 0
w6b:      .byte " DTR  <- HANG-UP", 0
w7a:      .byte "   GND ", 0
w7b:      .byte " GND", 0
w_swap1:  .byte "CABLE DCD -> 6551 DSR PIN (BIT 6)", 0
w_swap2:  .byte "CABLE DSR -> 6551 DCD PIN (BIT 5)", 0

; help page: 4 key rows (prmenu syntax) + 6 how-to rows
hk_lo:    .byte <hk1, <hk2, <hk3, <hk4
hk_hi:    .byte >hk1, >hk2, >hk3, >hk4
hk1:      .byte "[D] TOGGLE DTR     [R] TOGGLE RTS", 0
hk2:      .byte "[B] NEXT BAUD      [I] RESET ACIA 8N1", 0
hk3:      .byte "[T] SEND 16 X $55  [A] SEND AT + CR", 0
hk4:      .byte "[C] CLEAR LOG      [Q] QUIT TO BASIC", 0
hh_lo:    .byte <hh1, <hh2, <hh3, <hh4, <hh5, <hh6
hh_hi:    .byte >hh1, >hh2, >hh3, >hh4, >hh5, >hh6
hh1:      .byte "1 PC: TCPSER -D /DEV/TTYUSB0 -S 2400", 0
hh2:      .byte "2 PRESS A. OK IN LAST RX = BAUD OK.", 0
hh3:      .byte "  ELSE PRESS B AND RETRY. FRAMING", 0
hh4:      .byte "  ERRS > 0 MEANS THE BAUD IS WRONG.", 0
hh5:      .byte "3 TOGGLE D/R: WATCH TCPSER'S CONTROL", 0
hh6:      .byte "  LINE LOG TO MAP THE CABLE. F3=LOG", 0

l_dcd:    .byte "DCD:", 0
l_dsr:    .byte " DSR:", 0
l_dtr:    .byte " DTR:", 0
l_rts:    .byte " RTS:", 0
l_rx:     .byte "RX:", 0
l_tx:     .byte "  TX:", 0
l_err:    .byte "  ERR:", 0
l_txc:    .byte "TX ", 0
l_rxc:    .byte "  RX ", 0
l_frc:    .byte "  FRAMING ", 0

; NOTE: character set 1 renders lowercase ASCII as graphics, so every state
; word is uppercase; "off" states are distinguished by colour (grey).
s_carrier:.byte "CARRIER", 0
s_nocar:  .byte "NONE   ", 0
s_ready:  .byte "READY", 0
s_off5:   .byte "OFF  ", 0
s_on3:    .byte "ON ", 0
s_off3:   .byte "OFF", 0
s_data:   .byte "DATA", 0
s_idle:   .byte "IDLE", 0
s_busy:   .byte "BUSY ", 0
s_par:    .byte " PAR", 0
s_frm:    .byte " FRM", 0
s_ovr:    .byte " OVR", 0

m_hello:  .byte "READY. PRESS A TO SEND AT TO TCPSER.", 0
m_dtr_on: .byte "DTR ASSERTED", 0
m_dtr_off:.byte "DTR DROPPED", 0
m_rts_on: .byte "RTS ASSERTED", 0
m_rts_off:.byte "RTS DROPPED", 0
m_baud:   .byte "BAUD CHANGED, CHIP REPROGRAMMED", 0
m_init:   .byte "ACIA RESET: 8N1, DTR+RTS ON, NO IRQ", 0
m_clear:  .byte "LOG AND COUNTERS CLEARED", 0
m_test:   .byte "SENT 16 X $55 (U) AUTO-BAUD PATTERN", 0
m_at:     .byte "SENT AT+CR. WATCH LAST RX FOR OK.", 0
m_txfail: .byte "TX TIMEOUT: TRANSMITTER NOT READY", 0
m_noacia: .byte "NO 6551 DETECTED", 0
m_bye:    .byte "73! LINK ANALYZER DONE.", $0d, 0
