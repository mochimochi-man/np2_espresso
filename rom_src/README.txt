Sources for the two ROMs built into the firmware (main/rom/).

BIOS_ESP.ROM   <- bios_esp.asm, via build_bios_esp.sh
    BIOS98C, a compatible PC-98 boot BIOS. It is built around np2kai's hook
    mechanism: it points the interrupt vectors at the addresses bios.c
    dispatches on, plants the hook instruction at each, and calls
    bootstrapload() itself. Roughly 300 bytes of code in a 96KB window - np2kai
    loads the BIOS with

        biosrom = (file_read(fh, mem + 0x0e8000, 0x18000) == 0x18000);

    so the file has to be exactly 0x18000 bytes. Needs nasm.

FONT_ESP.ROM   <- mkfont_esp.py
    A PC-98 FONT.ROM (T98-Next layout) built from the Shinonome bitmap fonts,
    which are public domain. Needs pcf2bdf and the xfonts-shinonome package.

Neither contains any NEC code or data. They exist so that a board with a blank
microSD card still boots; a BIOS.ROM or FONT.ROM on the card is used in
preference to them, and one named in the menu wins over both.

NEC's own MS-DOS identifies the machine from a signature in the BIOS window and
will not run without it. BIOS98C does not carry one, so that DOS needs a
BIOS.ROM dumped from an NEC machine. EPSON DOS, and everything that does not
check, are unaffected.
