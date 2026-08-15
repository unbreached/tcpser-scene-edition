; ===========================================================================
;  TCPSER LINK ANALYZER  (C64 side)  v0.1
;  Interactive RS232 adapter / cable diagnostic that pairs with tcpser.
;
;  - Probes for a 6551 ACIA (SwiftLink / Turbo232 / GLINK232) at $DE00.
;  - Shows a LIVE readout of the 6551 status + command registers, decoded to
;    the individual handshake bits (DCD, DSR, DTR, RTS, RX/TX ready, errors).
;  - Interactive: toggle DTR / RTS and send a test byte, to map cable wiring
;    against tcpser's control-line monitor.
;
;  Assemble WITHOUT -t c64 (keep raw ASCII in string literals):
;     ca65 tcpser-link.s -o tcpser-link.o
;     ld65 -C c64prg.cfg -o tcpser-link.prg tcpser-link.o
; ===========================================================================

; ---- KERNAL -----
CHROUT  = $FFD2
GETIN   = $FFE4
PLOT    = $FFF0        ; carry clear: set cursor row=X col=Y

; ---- 6551 ACIA (SwiftLink default base) -----
ACIA    = $DE00
A_DATA  = ACIA+0
A_STAT  = ACIA+1       ; read: status ; write: programmed reset
A_CMD   = ACIA+2       ; command register
A_CTRL  = ACIA+3       ; control register

; ---- VIC colors -----
VBORDER = $D020
VBG     = $D021

; ---- zero page scratch -----
ptr     = $FB          ; string pointer
tmp     = $FD

; ---- PETSCII codes -----
RVON    = $12
RVOFF   = $92
WHITE   = $05
CYAN    = $9F
YELLOW  = $9E
GREEN   = $1E
LGREEN  = $99
RED     = $1C
GREY    = $98
CLS     = $93

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

        jsr detect_acia        ; sets have_acia (0/1)
        jsr draw_frame         ; static UI

mainloop:
        jsr refresh_regs       ; live register readout
        jsr handle_key         ; poll keyboard
        jsr delay
        jmp mainloop

; ===================== adapter detection ==================================
; Probe: write two patterns to the control register and read back. A real
; 6551 latches them; open bus does not. Control reg doesn't move handshake
; lines, so this is side-effect free on the cable.
detect_acia:
        lda #$16
        sta A_CTRL
        lda A_CTRL
        cmp #$16
        bne @no
        lda #$1e
        sta A_CTRL
        lda A_CTRL
        cmp #$1e
        bne @no
        lda #1
        sta have_acia
        rts
@no:    lda #0
        sta have_acia
        rts

; ===================== static frame =======================================
draw_frame:
        ; --- title bar (reverse cyan) ---
        ldx #0
        ldy #0
        jsr gotoxy
        lda #RVON
        jsr CHROUT
        lda #CYAN
        jsr CHROUT
        lda #<t_title
        ldy #>t_title
        jsr prstr
        lda #RVOFF
        jsr CHROUT

        ; --- adapter line ---
        ldx #2
        ldy #0
        jsr gotoxy
        lda #WHITE
        jsr CHROUT
        lda #<t_adapter
        ldy #>t_adapter
        jsr prstr
        lda have_acia
        bne @yes
        lda #RED
        jsr CHROUT
        lda #<t_none
        ldy #>t_none
        jsr prstr
        jmp @hdr
@yes:   lda #LGREEN
        jsr CHROUT
        lda #<t_swift
        ldy #>t_swift
        jsr prstr

@hdr:   ; --- section header: registers ---
        ldx #4
        ldy #0
        jsr gotoxy
        lda #RVON
        jsr CHROUT
        lda #YELLOW
        jsr CHROUT
        lda #<t_regs
        ldy #>t_regs
        jsr prstr
        lda #RVOFF
        jsr CHROUT

        ; static labels
        ldx #5
        ldy #0
        jsr gotoxy
        lda #WHITE
        jsr CHROUT
        lda #<t_status
        ldy #>t_status
        jsr prstr
        ldx #7
        ldy #0
        jsr gotoxy
        lda #<t_cmd
        ldy #>t_cmd
        jsr prstr

        ; --- section header: controls ---
        ldx #11
        ldy #0
        jsr gotoxy
        lda #RVON
        jsr CHROUT
        lda #YELLOW
        jsr CHROUT
        lda #<t_ctlhdr
        ldy #>t_ctlhdr
        jsr prstr
        lda #RVOFF
        jsr CHROUT

        ldx #12
        ldy #0
        jsr gotoxy
        lda #WHITE
        jsr CHROUT
        lda #<t_menu1
        ldy #>t_menu1
        jsr prstr
        ldx #13
        ldy #0
        jsr gotoxy
        lda #<t_menu2
        ldy #>t_menu2
        jsr prstr

        ; hint line
        ldx #16
        ldy #0
        jsr gotoxy
        lda #GREY
        jsr CHROUT
        lda #<t_hint
        ldy #>t_hint
        jsr prstr
        rts

; ===================== live register readout ==============================
refresh_regs:
        ; STATUS row 5, col 16:  "$xx"  then bits row 6
        ldx #5
        ldy #16
        jsr gotoxy
        lda #CYAN
        jsr CHROUT
        lda #'$'
        jsr CHROUT
        lda A_STAT
        sta tmp
        jsr prhex

        ; decoded status bits, row 6 col 2
        ldx #6
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr CHROUT
        lda #<l_dcd
        ldy #>l_dcd
        jsr prstr
        lda tmp
        and #$20              ; bit5 DCD
        jsr pronoff
        lda #<l_dsr
        ldy #>l_dsr
        jsr prstr
        lda tmp
        and #$40              ; bit6 DSR
        jsr pronoff
        lda #<l_rx
        ldy #>l_rx
        jsr prstr
        lda tmp
        and #$08              ; bit3 RDRF
        jsr pronoff
        lda #<l_tx
        ldy #>l_tx
        jsr prstr
        lda tmp
        and #$10              ; bit4 TDRE
        jsr pronoff

        ; CMD row 7 col 16
        ldx #7
        ldy #16
        jsr gotoxy
        lda #CYAN
        jsr CHROUT
        lda #'$'
        jsr CHROUT
        lda A_CMD
        sta tmp
        jsr prhex

        ; decoded cmd bits row 8 col 2
        ldx #8
        ldy #2
        jsr gotoxy
        lda #WHITE
        jsr CHROUT
        lda #<l_dtr
        ldy #>l_dtr
        jsr prstr
        lda tmp
        and #$01              ; bit0 DTR
        jsr pronoff
        lda #<l_rts
        ldy #>l_rts
        jsr prstr
        lda tmp
        and #$0c              ; bits2-3 tx/RTS ctrl
        jsr pronoff
        rts

; A (masked) != 0 -> print green "ON " else grey "off"
pronoff:
        cmp #0
        beq @off
        pha
        lda #LGREEN
        jsr CHROUT
        lda #<s_on
        ldy #>s_on
        jsr prstr
        pla
        rts
@off:   lda #GREY
        jsr CHROUT
        lda #<s_off
        ldy #>s_off
        jsr prstr
        lda #WHITE
        jsr CHROUT
        rts

; ===================== keyboard ===========================================
handle_key:
        jsr GETIN
        cmp #0
        beq @done
        cmp #'D'
        beq key_dtr
        cmp #'R'
        beq key_rts
        cmp #'T'
        beq key_test
        cmp #'Q'
        beq key_quit
@done:  rts

key_dtr:
        lda have_acia
        beq @m
        lda A_CMD
        eor #$01              ; flip DTR bit
        sta A_CMD
        lda #<m_dtr
        ldy #>m_dtr
        jmp msg
@m:     jmp msg_noacia

key_rts:
        lda have_acia
        beq @m
        lda A_CMD
        eor #$08              ; flip a tx/RTS control bit
        sta A_CMD
        lda #<m_rts
        ldy #>m_rts
        jmp msg
@m:     jmp msg_noacia

key_test:
        lda have_acia
        beq @m
        lda #$55             ; 'U' = alternating bits (auto-baud pattern)
        sta A_DATA
        lda #<m_test
        ldy #>m_test
        jmp msg
@m:     jmp msg_noacia

key_quit:
        lda #CLS
        jsr CHROUT
        lda #WHITE
        jsr CHROUT
        lda #<m_bye
        ldy #>m_bye
        jsr prstr
@spin:  jmp @spin

msg_noacia:
        lda #<m_noacia
        ldy #>m_noacia
        ; fall through to msg
; msg: print a message on the status line (row 14). A=lo, Y=hi of string.
msg:
        sta msgptr
        sty msgptr+1
        ; clear line 14 (40 spaces)
        ldx #14
        ldy #0
        jsr gotoxy
        lda #YELLOW
        jsr CHROUT
        ldx #39
@clr:   lda #' '
        jsr CHROUT
        dex
        bne @clr
        ; reposition and print the message
        ldx #14
        ldy #0
        jsr gotoxy
        lda #YELLOW
        jsr CHROUT
        lda msgptr
        ldy msgptr+1
        jsr prstr
        rts

; ===================== helpers ============================================
; print null-terminated string: A=lo, Y=hi
prstr:
        sta ptr
        sty ptr+1
        ldy #0
@l:     lda (ptr),y
        beq @e
        jsr CHROUT
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
@o:     jsr CHROUT
        rts

delay:
        ldx #0
        ldy #0
@l:     inx
        bne @l
        iny
        cpy #$30
        bne @l
        rts

; ===================== data ===============================================
have_acia: .byte 0
msgptr:    .word 0

t_title:  .byte " TCPSER LINK ANALYZER  v0.1            ", 0
t_adapter:.byte "ADAPTER: ", 0
t_swift:  .byte "6551 ACIA @ $DE00 (SWIFTLINK)", 0
t_none:   .byte "NONE AT $DE00 (CHECK CARTRIDGE)", 0
t_regs:   .byte " 6551 REGISTERS (LIVE)                ", 0
t_status: .byte "STATUS  $DE01 = ", 0
t_cmd:    .byte "COMMAND $DE02 = ", 0
t_ctlhdr: .byte " CONTROLS                             ", 0
t_menu1:  .byte "[D] TOGGLE DTR    [R] TOGGLE RTS", 0
t_menu2:  .byte "[T] SEND TEST U   [Q] QUIT", 0
t_hint:   .byte "WATCH TCPSER: TOGGLE DTR/RTS TO MAP WIRING", 0

l_dcd:    .byte "DCD:", 0
l_dsr:    .byte " DSR:", 0
l_rx:     .byte " RXF:", 0
l_tx:     .byte " TXE:", 0
l_dtr:    .byte "DTR:", 0
l_rts:    .byte " RTS:", 0
s_on:     .byte "ON ", 0
s_off:    .byte "off", 0

m_dtr:    .byte "DTR TOGGLED", 0
m_rts:    .byte "RTS TOGGLED", 0
m_test:   .byte "SENT TEST BYTE $55 (U)", 0
m_noacia: .byte "NO 6551 DETECTED", 0
m_bye:    .byte "73! LINK ANALYZER DONE.", 0
