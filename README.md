# SidecarTridge Multi-device Microfirmware App template

This is the template to create a Microfirmware app for the SidecarTridge Multidevice-app for Atari ST computers.

# ⚠️ ATTENTION! READ THIS FIRST

The process for creating a microfirmware app from this template is now documented in the official [SidecarTridge Multi-device documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/). To avoid inconsistencies and outdated information, we've centralized the instructions there. Please refer to the official documentation for the latest guidance.

## Shared 64 KB region layout

The template now ships with a single source-of-truth layout for the 64 KB shared region (m68k `$FA0000`–`$FAFFFF`, mirrored at RP `0x20030000`):

- The cartridge image (m68k header + code) lives in the first **8 KB** (`$FA0000`–`$FA1FFF`). `target/atarist/build.sh` enforces this with a hard size check on `BOOT.BIN`.
- A small fixed-offset metadata block (`CMD_MAGIC_SENTINEL`, `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4-byte indexed shared variables) sits at `$FA2000`.
- The **APP_FREE** arena (~48 KB at `$FA2300`) is the contiguous space your app should use for its own buffers.
- The template's **framebuffer** offset (8000 B at `$FAE0C0`, the top of the region) is still defined, but this app draws nothing there: it has no RP-drawn menu.

Both sides derive every offset symbolically from the constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). Apps must never hard-code an address inside the region — always reference the named offset/symbol so the layout stays the single source of truth.

See `programming.md` for the full table and the budget rules.

## Cartridge code

This app serves the disks of the Jackintosh ROMs, which run with no TOS and never run the cartridge's code. Under TOS the cartridge, `target/atarist/src/main.s` (all 8 KB, placed by `target/atarist/src/userfw.ld`), only shows a boot screen written with GEMDOS calls: X starts Booster, G continues to the GEM desktop, and after 30 seconds Booster starts on its own; any other key stops the countdown. Every ST boot shows it again.

Adding an m68k module follows the `gemdrive.ld`-style pattern used by `md-drives-emulator`: give `main.s` less of the 8 KB, place the new `.text` section at a fixed offset in `userfw.ld`, mirror the offset with an `equ` in `inc/sidecart_layout.s`, and add the `.o` target to `target/atarist/Makefile`.

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file. 
