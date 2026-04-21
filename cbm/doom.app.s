; doom.app.s  –  IDUN Doom C64-side application
;
; Assembled with ACME.  Load/run address: $6000.
; Sources the standard IDUN kernel header so the IDUN shell can launch it
; with:  go doom
;
; Memory layout used:
;   $6000-$7FFF  this code + data
;   $8000-$83E7  VIC screen RAM  (bank 2, offset $0000)
;   $A000-$BF3F  VIC bitmap      (bank 2, offset $2000)
;   $D800-$DBE7  Color RAM       (always here, outside VIC bank)
;
; Protocol (Pi -> C64 via $DE00):
;   [0x01] [count_lo] [count_hi]                     frame header
;   per dirty block:
;     [idx_lo] [idx_hi] [bm0..bm7] [screen] [color]  12 bytes / block
;   [audio_count] [samples...]                        audio (v1: always 0)
;
; Protocol (C64 -> Pi via $DE00):
;   [0x10] [joy] [keys]          input packet (sent each frame)
;
; Copyright (c) 2026 IDUN Doom contributors  –  GPLv3

; ── IDUN kernel includes ─────────────────────────────────────────────────────
; The IDUN build system makes these available via -I cbm/sys
!source "../cbm/sys/acehead.asm"

; ── constants ────────────────────────────────────────────────────────────────
idDataport  = $DE00          ; write=Pi-rx, read=Pi-tx
idRxBufLen  = $DE01          ; bytes waiting for C64 to read

vicBank     = $8000          ; VIC bank 2
screenBase  = $8000          ; screen RAM within bank
bitmapBase  = $A000          ; bitmap within bank
colorBase   = $D800          ; color RAM (always $D800)

CIA2_PORTA  = $DD00
CIA2_DDRA   = $DD02
CIA1_PORTB  = $DC01          ; joystick port 2

VIC_CTRL1   = $D011          ; $3B = bitmap, 25 rows, DEN
VIC_CTRL2   = $D016          ; $18 = multicolor, 40 cols
VIC_MEMCTL  = $D018          ; $08 = screen@+0, bitmap@+$2000
VIC_BGCOL   = $D021          ; background color

FRAME_MARKER    = $01
INPUT_MARKER    = $10

; ── zero-page variables ───────────────────────────────────────────────────────
; Use $22-$2F (free in most C64 configurations; IDUN apps get $60-$6C too)
zpPtr       = $22            ; 2 bytes – general destination pointer
zpLen       = $24            ; 1 byte  – byte counter
zpIdxLo     = $25            ; block index low byte
zpIdxHi     = $26            ; block index high byte
zpBlkCnt    = $27            ; 2 bytes – block count remaining
zpBlkCntH   = $28
zpJoy       = $29            ; last joystick reading (inverted)
zpKeys      = $2A            ; key flags

; ── ACME origin / PRG load address ───────────────────────────────────────────
* = $5FFE
!word $6000                  ; PRG two-byte load address

; ═══════════════════════════════════════════════════════════════════════════════
; APP ENTRY POINT  ($6000)
; The IDUN shell calls here after loading the PRG.
; We first start the Pi-side Lua script (which launches doom-idun),
; then set up the display, and finally enter the main loop.
; ═══════════════════════════════════════════════════════════════════════════════
* = $6000
appEntry:
    sei
    ; ── open Pi-side Lua script via IDUN kernel ─────────────────────────────
    ; kernel open("l:.d/main.lua", "r") starts main.lua on the Pi
    lda #<luaPath
    sta zp
    lda #>luaPath
    sta zp+1
    lda #<modeStr
    sta zw
    lda #>modeStr
    sta zw+1
    jsr aceFileOpen          ; returns fd in A; we ignore it (TTY always $DE00)

    ; ── set up VIC-II for multicolor bitmap ─────────────────────────────────
    jsr setupVideo

    ; ── clear screen / bitmap / color RAM ───────────────────────────────────
    jsr clearVideoMem

    cli
    ; ── main loop ─────────────────────────────────────────────────────────────
mainLoop:
    jsr waitFrameMarker      ; spin until 0x01 arrives
    jsr receiveFrame         ; blit all dirty blocks
    jsr sendInput            ; send joystick + key state to Pi
    jmp mainLoop

; ── strings ───────────────────────────────────────────────────────────────────
luaPath !pet "l:.d/main.lua", 0
modeStr !pet "r", 0

; ═══════════════════════════════════════════════════════════════════════════════
; setupVideo – configure VIC-II for multicolor bitmap mode
; ═══════════════════════════════════════════════════════════════════════════════
setupVideo:
    ; CIA2 port A: select VIC bank 2 ($8000) → bits 1-0 = %01 (inverted)
    lda CIA2_DDRA
    ora #$03
    sta CIA2_DDRA
    lda CIA2_PORTA
    and #$FC
    ora #$01
    sta CIA2_PORTA

    ; VIC memory control: screen at bank+$0000, bitmap at bank+$2000
    lda #$08
    sta VIC_MEMCTL

    ; Bitmap mode + 25 rows + display enable
    lda #$3B
    sta VIC_CTRL1

    ; Multicolor mode + 40 columns
    lda #$18
    sta VIC_CTRL2

    ; Background = black
    lda #$00
    sta VIC_BGCOL

    ; Border = black
    sta $D020

    rts

; ═══════════════════════════════════════════════════════════════════════════════
; clearVideoMem – zero screen RAM, bitmap, and color RAM
; ═══════════════════════════════════════════════════════════════════════════════
clearVideoMem:
    ; clear screen RAM ($8000, 1000 bytes)
    lda #$00
    ldx #0
-   sta screenBase, x
    sta screenBase+$100, x
    sta screenBase+$200, x
    dex
    bne -
    ; last 232 bytes of screen RAM (1000 - 768 = 232)
    ldx #231
-   sta screenBase+$300, x
    dex
    bpl -

    ; clear color RAM ($D800, 1000 bytes)
    ldx #0
-   sta colorBase, x
    sta colorBase+$100, x
    sta colorBase+$200, x
    dex
    bne -
    ldx #231
-   sta colorBase+$300, x
    dex
    bpl -

    ; clear bitmap ($A000, 8000 bytes = 32 pages of 256 bytes minus 32 bytes)
    ; do 31 full pages + last partial
    ldx #0
    lda #>bitmapBase
    sta zpPtr+1
    lda #<bitmapBase
    sta zpPtr
clrBmPage:
    lda #0
    ldy #0
-   sta (zpPtr), y
    iny
    bne -
    inc zpPtr+1
    lda zpPtr+1
    cmp #>bitmapBase + 31
    bne clrBmPage
    ; remaining 64 bytes (8000 - 31*256 = 8000 - 7936 = 64)
    ldy #63
-   sta (zpPtr), y
    dey
    bpl -
    rts

; ═══════════════════════════════════════════════════════════════════════════════
; waitByte – wait until idRxBufLen > 0, then return byte in A
; ═══════════════════════════════════════════════════════════════════════════════
waitByte:
-   lda idRxBufLen
    beq -
    lda idDataport
    rts

; ═══════════════════════════════════════════════════════════════════════════════
; waitFrameMarker – discard bytes until 0x01 seen
; ═══════════════════════════════════════════════════════════════════════════════
waitFrameMarker:
-   jsr waitByte
    cmp #FRAME_MARKER
    bne -
    rts

; ═══════════════════════════════════════════════════════════════════════════════
; receiveFrame – read dirty-block count, then blit each block
; ═══════════════════════════════════════════════════════════════════════════════
receiveFrame:
    ; read block count (little-endian 16-bit)
    jsr waitByte
    sta zpBlkCnt
    jsr waitByte
    sta zpBlkCntH

    ; if count == 0 skip blocks but still read audio byte
    lda zpBlkCnt
    ora zpBlkCntH
    beq noBlocks

blkLoop:
    ; read block index (16-bit LE)
    jsr waitByte
    sta zpIdxLo
    jsr waitByte
    sta zpIdxHi

    ; compute and blit this block
    jsr blitBlock

    ; decrement 16-bit counter
    lda zpBlkCnt
    bne +
    dec zpBlkCntH
+   dec zpBlkCnt
    lda zpBlkCnt
    ora zpBlkCntH
    bne blkLoop

noBlocks:
    ; read audio_count (v1: always 0, but consume it)
    jsr waitByte
    ; if > 0 we'd need to store samples; for now just drain
    tax
    beq doneAudio
drainAudio:
    jsr waitByte
    dex
    bne drainAudio
doneAudio:
    rts

; ═══════════════════════════════════════════════════════════════════════════════
; blitBlock – write bitmap(8), screen(1), color(1) for block in zpIdxLo/Hi
;
; Block n:
;   bitmap  addr = $A000 + n*8
;   screen  addr = $8000 + n
;   color   addr = $D800 + n
;
; n*8 arithmetic (n is 10-bit, 0-999):
;   lo = (n_lo << 3) & $FF        — low byte of n*8
;   hi = (n_hi << 3)|(n_lo >> 5)  — high byte of n*8
;   bitmap ptr = ($A0 + hi) : lo
;
; Screen and color pointers are just $80:n_lo/$D8:n_lo with n_hi in high byte.
; ═══════════════════════════════════════════════════════════════════════════════
blitBlock:
    ; ── compute bitmap pointer ────────────────────────────────────────────────
    ; save zpIdxLo bits 7-5 to form high byte of offset
    lda zpIdxLo
    lsr
    lsr
    lsr
    lsr
    lsr              ; A = zpIdxLo >> 5  (bits 7-5 of n, gives 0-7)
    sta zpPtr+1      ; temp store

    ; add contribution from zpIdxHi: high byte of n*8 = (n_hi<<3)|(n_lo>>5)
    lda zpIdxHi
    asl
    asl
    asl              ; zpIdxHi * 8
    ora zpPtr+1      ; combine with n_lo>>5
    clc
    adc #>bitmapBase ; add $A0
    sta zpPtr+1      ; final high byte of bitmap address

    lda zpIdxLo
    asl
    asl
    asl              ; zpIdxLo * 8 (low byte, carry discarded since we used >>5 above)
    sta zpPtr        ; low byte of bitmap address

    ; ── read and write 8 bitmap bytes ────────────────────────────────────────
    ldy #0
-   jsr waitByte
    sta (zpPtr), y
    iny
    cpy #8
    bne -

    ; ── compute screen RAM pointer ($8000 + n) ────────────────────────────────
    lda #<screenBase
    clc
    adc zpIdxLo
    sta zpPtr
    lda #>screenBase
    adc zpIdxHi
    sta zpPtr+1

    ; read and write screen byte
    jsr waitByte
    ldy #0
    sta (zpPtr), y

    ; ── compute color RAM pointer ($D800 + n) ─────────────────────────────────
    lda #<colorBase
    clc
    adc zpIdxLo
    sta zpPtr
    lda #>colorBase
    adc zpIdxHi
    sta zpPtr+1

    ; read and write color byte
    jsr waitByte
    ldy #0
    sta (zpPtr), y

    rts

; ═══════════════════════════════════════════════════════════════════════════════
; sendInput – read joystick port 2 and send [0x10, joy, keys] to Pi
; ═══════════════════════════════════════════════════════════════════════════════
sendInput:
    ; read joystick port 2 ($DC01, active low – invert so 1=pressed)
    lda CIA1_PORTB
    eor #$FF
    and #$1F             ; mask to 5 bits (up,down,left,right,fire)
    sta zpJoy

    ; for now no keyboard scanning – keys = 0
    lda #0
    sta zpKeys

    ; send 3-byte packet
    lda #INPUT_MARKER
    sta idDataport
    lda zpJoy
    sta idDataport
    lda zpKeys
    sta idDataport
    rts
