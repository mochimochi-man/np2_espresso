; -----------------------------------------------------------------------------
; BIOS98C - a compatible PC-98 boot BIOS for np2_espresso
;
; Clean-room: nothing here derives from NEC's ROM. The heavy lifting - disk I/O,
; the boot device scan, the interrupt services - is done by np2kai's own
; BSD-licensed C implementation, which the emulator runs whenever the CPU
; executes at one of its hook addresses. This ROM sets the machine up, plants a
; hook instruction at each of those addresses, and hands over.
;
; Scope: boot MS-DOS from a floppy or a hard disk. No BASIC, no graphics BIOS,
; no LIO - none of it is needed to reach a DOS prompt.
;
; Layout: np2kai loads the file to physical 0xE8000 and insists on exactly
; 0x18000 bytes (bios.c: `file_read(fh, mem + 0x0e8000, 0x18000) == 0x18000`),
; so the image is a flat 96KB covering 0xE8000-0xFFFFF. ORG is the physical
; address, which makes every label below a physical one too.
;
; Build: ./build.sh (nasm -f bin). Put the result on the SD card and point the
; emulator at it - it loads exactly the way a real BIOS.ROM does, so the two can
; be swapped for an A/B comparison.
; -----------------------------------------------------------------------------

        cpu     8086
        bits    16

ROM_BASE        equ 0x0e8000
ROM_SIZE        equ 0x018000
SEG_F000        equ 0x0f0000        ; everything jumped to lives in this segment

        org     ROM_BASE

; --- np2kai hook addresses --------------------------------------------------
; bios/bios.h: BIOS_SEG = 0xFD80, so BIOS_BASE = 0xFD800 and each service sits
; at BIOS_BASE + BIOSOFST_xx. Executing there runs the C routine (bios.c
; dispatches on the address); the byte we plant is the hook instruction, which
; defaults to NOP, followed by IRET for the return path.
BIOS_BASE       equ 0x0fd800
OFST_09         equ 0x0088          ; keyboard
OFST_0c         equ 0x008c          ; serial
OFST_12         equ 0x0090          ; FDC
OFST_13         equ 0x0094          ; FDC
OFST_18         equ 0x0098          ; common (CRT/keyboard)
OFST_19         equ 0x009c          ; RS-232C
OFST_1b         equ 0x00a8          ; disk  <- the one DOS lives on
OFST_1c         equ 0x00ac          ; timer
OFST_1f         equ 0x00b0          ; extended

HOOK_BOOTSTRAP  equ 0x0fffe8        ; bios.c:1176 - bootstrapload(), then jumps
                                    ; to CS=bootseg IP=0 when a device booted

; A far pointer to a physical address inside segment F000.
%define FARPTR(addr) ((addr) - SEG_F000)

; -----------------------------------------------------------------------------
; Cold entry, reached from the reset vector.
; -----------------------------------------------------------------------------
entry:
        cli
        cld

        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 0x0400          ; below the BIOS work area, above the IVT

        call    init_vectors
        call    init_biosmem

        ; Hand over to np2kai's boot device scan. AX=0 selects the "emulation
        ; BIOS" path in bootstrapload() (bios1b.c:839), which takes the boot
        ; order from memory switch 5 and, in the normal case, tries floppies 0-3
        ; and then the hard disks. On success it never comes back: the hook sets
        ; CS:IP to the loaded IPL itself.
        xor     ax, ax
        jmp     0xf000:FARPTR(HOOK_BOOTSTRAP)

; -----------------------------------------------------------------------------
; Interrupt vectors: point each one at its hook. Everything DOS touches during
; boot is here; the rest of the table stays zero, which is a clean fault rather
; than a jump into nowhere.
; -----------------------------------------------------------------------------
%macro SETVEC 2                     ; %1 = interrupt number, %2 = hook address
        mov     word [es:%1 * 4], FARPTR(%2)
        mov     word [es:%1 * 4 + 2], 0xf000
%endmacro

init_vectors:
        push    ax
        xor     ax, ax
        mov     es, ax
        SETVEC 0x09, BIOS_BASE + OFST_09
        SETVEC 0x0c, BIOS_BASE + OFST_0c
        SETVEC 0x12, BIOS_BASE + OFST_12
        SETVEC 0x13, BIOS_BASE + OFST_13
        SETVEC 0x18, BIOS_BASE + OFST_18
        SETVEC 0x19, BIOS_BASE + OFST_19
        SETVEC 0x1b, BIOS_BASE + OFST_1b
        SETVEC 0x1c, BIOS_BASE + OFST_1c
        SETVEC 0x1f, BIOS_BASE + OFST_1f
        pop     ax
        ret

; -----------------------------------------------------------------------------
; BIOS work area. np2kai's C side fills in most of it (and owns the memory
; switches at 0xA3FF2), so this only has to leave the boot path a sane state.
; TODO: once a floppy boots, check what DOS reads out of 0000:0400-05FF and set
; whatever it turns out to need - guessing here just hides the real failure.
; -----------------------------------------------------------------------------
init_biosmem:
        ret

; -----------------------------------------------------------------------------
; The hook stubs. Each has to sit at its exact address, so they are emitted by
; padding to the address and planting NOP (the instruction bios.c watches for)
; followed by IRET.
; -----------------------------------------------------------------------------
%macro HOOKSTUB 1
        times   (%1) - ($ - $$) - ROM_BASE db 0xff
        nop
        iret
%endmacro

        HOOKSTUB BIOS_BASE + OFST_09
        HOOKSTUB BIOS_BASE + OFST_0c
        HOOKSTUB BIOS_BASE + OFST_12
        HOOKSTUB BIOS_BASE + OFST_13
        HOOKSTUB BIOS_BASE + OFST_18
        HOOKSTUB BIOS_BASE + OFST_19
        HOOKSTUB BIOS_BASE + OFST_1b
        HOOKSTUB BIOS_BASE + OFST_1c
        HOOKSTUB BIOS_BASE + OFST_1f

; The bootstrap hook: a NOP for the emulator to catch. If it ever returns, no
; device was bootable - stop visibly instead of running into whatever follows.
; This build has no BASIC to fall back to, by design.
        times   HOOK_BOOTSTRAP - ($ - $$) - ROM_BASE db 0xff
        nop
boot_failed:
        hlt
        jmp     boot_failed

; -----------------------------------------------------------------------------
; Reset vector at 0xFFFF0, and pad the image to exactly 0x18000 bytes.
; -----------------------------------------------------------------------------
        times   0xffff0 - ($ - $$) - ROM_BASE db 0xff
        jmp     0xf000:FARPTR(entry)
        times   ROM_SIZE - ($ - $$) db 0xff
