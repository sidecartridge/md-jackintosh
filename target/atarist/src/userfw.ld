/* userfw.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR. Each module's .text section is placed at a fixed
 * offset within the image.
 *
 * Budget (must add up to <= CARTRIDGE_CODE_SIZE = 8 KB; the build
 * script asserts this against BOOT.BIN):
 *
 *   .text_main    0x0000..0x1FFF  ( 8 KB)  m68k boot + boot screen
 *
 * Adding a new module is a two-step change:
 *   1. Pick an offset within the cartridge budget and add a new
 *      ".text_<name> 0x????? : { <name>.o(.text) }" section here.
 *   2. Mirror the offset on the m68k side with `equ (ROM4_ADDR + $????)`
 *      in main.s, and add the .o target to target/atarist/Makefile.
 */
SECTIONS
{
    .text_main 0x000000 : {
        main.o(.text)
    }
}
