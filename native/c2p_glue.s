; C-callable wrapper around Mikael Kalms' dirty-rectangle c2p.
;
; c2p_rect takes all nine arguments in registers, including d2-d7 which the C
; ABI treats as callee-saved. It does save them itself - but only AFTER we have
; already loaded our arguments into them, so it would restore OUR values and
; destroy the caller's. Hence this wrapper saves them first.
;
; Assemble with the vasm that ships with bebbo amiga-gcc:
;   vasmm68k_mot -Fhunk -m68040 -no-opt -o c2p_glue.o c2p_glue.s

	section	code,code

	xdef	_c2p_rect_asm

; void c2p_rect_asm(struct C2PArgs *a);
;   0  UWORD x        (multiple of 32)
;   2  UWORD y
;   4  UWORD w        (multiple of 32)
;   6  UWORD h
;   8  UWORD cmod     chunky buffer stride in bytes
;  10  UWORD bmod     bitplane BytesPerRow
;  12  ULONG bplsize  bytes per bitplane (distance between planes)
;  16  APTR  chunky   BASE of the chunky buffer (routine adds x/y itself)
;  20  APTR  bpl      BASE of plane 0 (planes must be contiguous, bplsize apart)

_c2p_rect_asm:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a1		; 11 saved longs = 44, +4 return address
	move.w	(a1),d0
	move.w	2(a1),d1
	move.w	4(a1),d2
	move.w	6(a1),d3
	move.w	8(a1),d4
	move.w	10(a1),d5
	move.l	12(a1),d6
	move.l	16(a1),a0
	move.l	20(a1),a1
	jsr	c2p_rect
	movem.l	(sp)+,d2-d7/a2-a6
	rts

	include	"c2p_rect.s"
