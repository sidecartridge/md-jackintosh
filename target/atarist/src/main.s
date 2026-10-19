; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code
PRE_RESET_WAIT		equ $FFFFF

CMD_NOP				equ 0		; No operation command
CMD_RESET			equ 1		; Reset command

; Asks the RP to reset the ST and start Booster (EMUL_CMD_BOOSTER in
; rp/src/include/emul.h). No payload.
CMD_BOOSTER			equ $0002

_hz_200				equ $4BA	; The 200 Hz system timer
BOOT_SCREEN_SECONDS	equ 30		; The boot screen's countdown to Booster
BOOSTER_RESET_WAIT	equ (3 * 200)	; How long the RP gets to reset the ST, in ticks
SCREEN_MARGIN_80	equ 20		; The 40-column boot screen, centred on 80 columns
STATUS_ROW			equ 12		; The boot screen's countdown line

; The cartridge window's layout and the command channel, shared with every
; module that talks to the RP.
	include inc/sidecart_layout.s

	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; The Mega STE's setting as megaste_take found it, and its word off the stack.
; This runs from the copy in RAM: nothing PC-relative outside it.
megaste_hand_back	macro
					tst.b (sp)
					beq.s .\@megaste_none
					move.b 1(sp), MEGASTE_SPEED_CACHE_REG.w
.\@megaste_none:
					addq.l #2, sp
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $2000 (8 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "TERM",0
    even

pre_auto:
; On a Mega STE, the cache off until the cartridge hands over: the boot screen
; talks to the cartridge, and with the cache on its commands never reach the
; RP. The speed stays the user's. The word under the return address
; keeps the setting (megaste_take, megaste_hand_back).
	clr.w -(sp)
	bsr megaste_take

; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

; The word the caller pushed takes a flag and the Mega STE's speed and cache
; register, and the cache is turned off; the speed stays. Nothing has written
; the machine type into the shared variables yet, so the cookie says.
megaste_take:
	move.l _p_cookies.w, d0
	beq.s .megaste_take_done
	move.l d0, a0
.megaste_take_next:
	move.l (a0)+, d0
	beq.s .megaste_take_done
	cmp.l #'_MCH', d0
	beq.s .megaste_take_mch
	addq.w #4, a0
	bra.s .megaste_take_next
.megaste_take_mch:
	cmp.l #COOKIE_JAR_MEGASTE, (a0)
	bne.s .megaste_take_done
	st 4(sp)							; a setting to put back
	move.b MEGASTE_SPEED_CACHE_REG.w, 5(sp)
	bclr #0, MEGASTE_SPEED_CACHE_REG.w
.megaste_take_done:
	rts

start_rom_code:
; Tell the RP the ST has booted, and then which machine and TOS this is, in
; shared variables 0 and 1. The senders' Mega STE and 68030 checks read the
; machine from there. Both wait until the RP answers.
.hello:
	send_sync CMD_ST_HELLO, 0
	bne.s .hello
	bsr detect_hw
	bsr get_tos_version

; The boot screen, the cartridge's only job under TOS: it serves the
; Jackintosh ROMs, which never run this code. X starts Booster, G the GEM
; desktop, and after BOOT_SCREEN_SECONDS Booster starts on its own. Any other
; key stops the countdown, and then only X or G leave. GEMDOS keeps d3-d7
; and a3.
	move.w #4, -(sp)			; XBIOS Getrez
	trap #14
	addq.l #2, sp
	moveq #0, d7				; d7: the left margin, none on 40 columns
	tst.w d0
	beq.s .screen_margin
	moveq #SCREEN_MARGIN_80, d7
.screen_margin:
	pea screen_clear_msg(pc)
	gemdos Cconws,6
	lea screen_lines(pc), a3
	bsr print_lines
	move.l _hz_200.w, d5
	add.l #(BOOT_SCREEN_SECONDS * 200), d5	; When Booster starts
	moveq #-1, d4				; The seconds shown: none yet
	moveq #0, d6				; The countdown runs
.countdown:
	tst.w d6
	bne.s .countdown_keys		; Stopped: only the keys
	move.l d5, d3
	sub.l _hz_200.w, d3			; Ticks left
	ble.s .to_booster
	add.l #199, d3
	divu #200, d3				; Seconds left, rounded up
	cmp.w d3, d4
	beq.s .countdown_keys
	move.w d3, d4
	bsr print_countdown
.countdown_keys:
	gemdos Cconis,2
	tst.w d0
	beq.s .countdown
	gemdos Cnecin,2
	and.b #$DF, d0				; Upper case
	cmp.b #'X', d0
	beq.s .to_booster
	cmp.b #'G', d0
	beq.s .to_gem
	tst.w d6
	bne.s .countdown			; Already stopped
	moveq #1, d6
	lea stopped_line(pc), a3	; Over the countdown's line
	bsr print_lines
	bra.s .countdown
.to_gem:
	lea gem_line(pc), a3
	bsr print_lines
	pea cursor_on_msg(pc)
	gemdos Cconws,6
	bra boot_gem

; The RP resets the ST through the sentinel once it is ready to start Booster;
; if it never does, the ST resets anyway.
.to_booster:
	lea booster_line(pc), a3
	bsr print_lines
.booster_send:
	send_sync CMD_BOOSTER, 0
	bne.s .booster_send
	move.l _hz_200.w, d5
	add.l #BOOSTER_RESET_WAIT, d5
.booster_wait:
	cmp.l #CMD_RESET, CMD_MAGIC_SENTINEL_ADDR
	beq.s .reset
	cmp.l _hz_200.w, d5
	bhi.s .booster_wait

.reset:
    move.l #PRE_RESET_WAIT, d6
.wait_me:
    subq.l #1, d6           ; Decrement the outer loop
    bne.s .wait_me          ; Wait for the timeout

	clr.l $420.w			; Invalidate memory system variables
	clr.l $43A.w
	clr.l $51A.w
	move.l $4.w, a0			; Now we can safely jump to the reset vector
	jmp (a0)
	nop

boot_gem:
	; If we get here, continue loading GEM
	megaste_hand_back
    rts

; Print the seconds left, d4.w, over the ones shown before.
print_countdown:
	lea countdown_line(pc), a3
	bsr print_lines
	moveq #0, d3
	move.w d4, d3
	divu #10, d3				; Tens in the low word, units in the high word
	moveq #' ', d0
	tst.w d3
	beq.s .print_countdown_tens
	moveq #'0', d0
	add.w d3, d0
.print_countdown_tens:
	bsr print_char
	swap d3
	moveq #'0', d0
	add.w d3, d0
	bsr print_char
	pea seconds_msg(pc)
	gemdos Cconws,6
	rts

; Print the lines of the table at a3: each is a row, a column and a text
; ending in 0, and the table ends with a row of $FF. The column moves right by
; the margin in d7.
print_lines:
	move.b (a3)+, d0
	cmp.b #$FF, d0
	beq.s .print_lines_done
	move.b (a3)+, d1
	bsr goto_row_col
	move.l a3, -(sp)
	move.w #Cconws, -(sp)
	trap #1
	addq.l #6, sp
.print_lines_skip:
	tst.b (a3)+
	bne.s .print_lines_skip
	bra.s print_lines
.print_lines_done:
	rts

; Move the cursor to row d0.b, column d1.b plus the margin: VT52 ESC Y.
goto_row_col:
	movem.l d3-d4, -(sp)
	move.b d0, d3
	move.b d1, d4
	moveq #27, d0
	bsr print_char
	moveq #'Y', d0
	bsr print_char
	moveq #32, d0
	add.b d3, d0
	bsr print_char
	moveq #32, d0
	add.b d4, d0
	add.b d7, d0
	bsr print_char
	movem.l (sp)+, d3-d4
	rts

; Print the character in d0.w.
print_char:
	move.w d0, -(sp)
	move.w #Cconout, -(sp)
	trap #1
	addq.l #4, sp
	rts

screen_clear_msg:
	dc.b 27,"E",27,"f",0			; Clear the screen, cursor off
cursor_on_msg:
	dc.b 27,"e",0
; Row, column, text (see print_lines). 40 columns wide.
screen_lines:
	dc.b 0,0,27,"p"," SidecarTridge Multi-device             ",27,"q",0
	dc.b 2,2,"JACKINTOSH "
	include version.inc			; The version, written by the Makefile
	ifne _DEBUG
	dc.b " (debug build)"
	endif
	dc.b 0
	dc.b 4,2,"This cartridge works with the",0
	dc.b 5,2,"Jackintosh ROMs, which turn this",0
	dc.b 6,2,"Atari into a Macintosh Plus.",0
	dc.b 8,2,"[X]  Back to Booster",0
	dc.b 9,2,"[G]  Continue to the GEM desktop",0
	dc.b 10,2,"Any other key stops the countdown.",0
	dc.b 15,2,"Open jackintosh.sidecartridge.com",0
	dc.b 16,2,"to learn more.",0
	dc.b 24,8,"(C)2026 GOODDATA LABS SL",0
	dc.b $FF
countdown_line:
	dc.b STATUS_ROW,2,27,"K","Booster starts in ",0,$FF
seconds_msg:
	dc.b " s",0
stopped_line:
	dc.b STATUS_ROW,2,27,"K","Countdown stopped: press X or G",0,$FF
booster_line:
	dc.b STATUS_ROW,2,27,"K","Starting Booster...",0,$FF
gem_line:
	dc.b STATUS_ROW,2,27,"K","Starting the GEM desktop...",0,$FF
	even

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"

; The NOP tail. The senders' wait loop must never be the last code of a module:
; firmware.py strips trailing zero bytes from the image, the RP copies only
; that many words into the cartridge window, and both the write sender (its
; code size includes 4 bytes past the loop) and the 68000's prefetch read past
; the loop's last word. Every module that includes sidecart_functions.s ends
; like this.
	even
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
main_end:

end_rom_code:
end_pre_auto:
	even
	dc.l 0