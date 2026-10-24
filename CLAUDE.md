# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `programming.md` (the developer guide: the parts, the modules, memory, the cartridge window and the commands), `README.md` (what the app does, for its users), `AGENTS.md` (host setup, copy-pasteable commands, and a symptom → fix table), `rp/src/include/macdisk.h` (the contract with the Jackintosh ROMs).

## What this repo is

**md-jackintosh** is a SidecarTridge Multi-device microfirmware app for the Atari ST / STE / Mega ST(E): a UF2 image for the Raspberry Pi Pico W on the Multi-device in the cartridge port, built from the Multi-device microfirmware template (whose own history and generic docs live in that repository and at <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>).

It serves the floppy and hard disk images of a Mac emulator from the microSD card (`rp/src/macdisk.c`), with its clock and parameter RAM. The emulator is a boot ROM built in the sibling repo `md-jackintosh-roms` (and at <https://jackintosh.sidecartridge.com>), in the TOS sockets: served by the SidecarTridge TOS Emulator, or burnt into EPROMs. It turns the ST into a Mac Plus at power-on with **no TOS at all**, so nothing runs this cartridge's m68k code. Under TOS the cartridge only shows a boot screen that sends the user to Booster or the GEM desktop; there is no setup menu, and any menu the Mac needs lives in the Mac ROM. The ROM speaks the command protocol itself, starting with `CMD_ST_HELLO`. Don't make the RP side depend on `BOOT.BIN` having run, and keep the protocol and the `chandler.h` window layout unchanged: together with `rp/src/include/macdisk.h` they are the contract with that repo. Change any of them only after agreeing it with `md-jackintosh-roms`.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb
# <build_type> = release | debug, any case; anything else stops the build (both CMake Release — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 123e4567-e89b-12d3-a456-426614174000
```

Required host environment:
- ARM GNU Toolchain 14.2 — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
- `atarist-toolkit-docker` (`stcmd`) — needed for the m68k target. It runs `docker run -it`, so it needs a TTY unless `STCMD_NO_TTY=1` is exported (the build scripts set it; see the gotcha below).
- SDK paths (auto-set from the repo if unset): `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`.

Build flow (orchestrated by `build.sh`). Every script stops at the first failing step (`set -Eeo pipefail`) with `ERROR: <script>: failed at line N`, and `build.sh` empties `dist/` before anything else, so a failed build leaves no UF2 or JSON behind:
1. Copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces an **8 KB hard limit** on `BOOT.BIN` (the cartridge code budget — `CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); a build that exceeds it aborts with `ERROR: cartridge code is N bytes; limit is 8192`. A separate copy (`FIRMWARE.IMG`) is then padded to 64 KB to fill the entire shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk v3.6.2), runs CMake, produces `rp/dist/rp-<board>.uf2`. The FatFs configuration lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

### Build gotchas
- **Both build types are CMake `Release` (-O3).** `<build_type>` sets only `DEBUG_MODE` for both targets (`DPRINTF`, UART stdio and the SWD hooks on the RP, `_DEBUG` in the m68k assembly) and the dist filename, so a debug build runs the same optimised code as a release build and every harness tests what ships. Keep `tprotocol.h`'s inline `strh` on low registers (`"l"`: Thumb-1's `strh` cannot use `ip`), or a debug build does not assemble. `RP_CMAKE_BUILD_TYPE` overrides the CMake type in `rp/build.sh` and `tools/dev/flash.sh`, with a warning: `MinSizeRel` for a smaller image, `Debug` (-Og, and asserts and lwIP's debug checks on, since it lacks `-DNDEBUG`) to step through the code in a debugger. Such a build's ID carries its type (`+minsizerel`, `+cmakedebug`).
- `RELEASE_DATE="YYYY-MM-DD HH:MM:SS"` fixes the date the images carry (the RP's debug banner, and the cartridge header's GEMDOS date and time), so two builds of one commit are byte-identical. Unset, it is the time of the build.
- `.vscode/cmake-variants.yaml` gives VS Code's CMake Tools the same two builds (CMake Release, `DEBUG_MODE`, the development UUID `44444444-4444-4444-8444-444444444444`). With no `RELEASE_VERSION` in the environment, CMake reads `rp/version.txt`.
- A stale `.git/modules/<submodule>/index.lock` stops the build at the pin step; if no git process is running, delete the lock.
- `CHARACTER_GAP_MS` must remain defined (700) in `rp/src/include/blink.h` — removing it breaks the RP build.
- `rp/build.sh` runs `git submodule update --init --recursive` and hard-`checkout`s the pinned revisions on **every** build, and `build.sh` deletes and recreates `dist/` first — any local edit inside a submodule, or anything left in `dist/`, is gone on the next build.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- VASM/`stcmd` errors like `the input device is not a TTY` mean `stcmd` was invoked without a PTY. `target/atarist/build.sh` already exports `STCMD_NO_TTY=1` for every `stcmd` call it makes; you only need to export it yourself if invoking `stcmd` directly from a non-TTY context (CI, sub-shells, build wrappers). Without it `stcmd` fails. The build scripts stop on that; a `stcmd make` run by hand that fails leaves the previous `BOOT.BIN` in place, and an RP build from it displays garbage on the ST because `target_firmware.h` is stale.

### CI / release
- `.github/workflows/build.yml` builds `pico_w` on every pull request, release and debug.
- `.github/workflows/release.yml` triggers on `v*` tags: builds, attaches UF2 + JSON to the GitHub Release, uploads to `s3://atarist.sidecartridge.com/`.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag (which triggers release). It first runs `make check-release`, which stops unless `ACONFIG_VERSION_NUMBER` in `rp/src/include/aconfig.h` is 1: it is 2 while developing, and the repo owner sets it back to 1 on `main` before tagging. **Don't tag**: a change lands as a `version.txt` bump plus a `CHANGELOG.md` entry; tagging and releasing are the repo owner's call.
- The app's release UUID is the `APP_UUID` secret of the GitHub repository, and only `release.yml` uses it, on the tag `make tag` pushes. Every other build (`build.yml` on pull requests, `tools/dev/flash.sh`, local `build.sh` runs) uses the development UUID `44444444-4444-4444-8444-444444444444`, so there is no release-UUID build to make or test before the tag.
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

### Tests / verification
There is no test suite, and the RP side cannot run off-hardware. "Verification" is: the build succeeds (including the 8 KB cartridge assertion), the UF2 boots on the device, and you check it over the serial debug console and on the ST (the boot screen under TOS).

- `DPRINTF` / `DPRINTFRAW` (`rp/src/include/debug.h`) compile to nothing unless `_DEBUG != 0`, and `pico_enable_stdio_uart` is only turned on for debug builds — so a `release` UF2 prints nothing at all. Build with `./build.sh <board> debug <uuid>` to get a serial console. If a release build ever prints, check `-D_DEBUG=` in its `flags.make`.
- On-chip debugging is via a Picoprobe/Debug Probe and the `cortex-debug` VS Code launch config in `.vscode/launch.json`; it needs `ARM_GDB_PATH` and `PICO_OPENOCD_PATH` exported. CMake source dir is `rp/src`, build dir is `rp/build`.
- The m68k side has no debugger: its only observable output is what it prints on the ST screen. The boot screen runs in Hatari (TOS 1.04, `--cartridge` with `dist/BOOT.BIN`, `--conout 2` to echo the TOS console), but Hatari has no Multi-device, so run a scratch copy of `main.s` without the hello and `detect_hw`, which wait for the RP forever. Start Hatari with `HOME` set to an empty folder: a user `hatari.cfg` with GEMDOS HD emulation on makes Hatari refuse the cartridge.

With a Raspberry Pi Debug Probe attached, `tools/dev/` turns that manual loop into something
repeatable (its `README.md` has the full command list):

- `console.py watch` captures the debug UART (921,600 baud) to `tools/dev/logs/console.log` with
  timestamps, and `since-boot` / `grep` / `wait` query the log while it runs. Only one program can
  hold the port, so use this instead of a serial terminal; other tools read the log, not the port.
  `wait` only matches lines that arrive after it starts.
- `flash.sh <debug|release>` builds out of tree in `tools/dev/builds/` incrementally (seconds, not
  a full rebuild), flashes over USB or the probe, then verifies over SWD that the RP booted the
  ELF, that its flash matches byte for byte, and that it carries the expected build ID.
- `swd.py` reads a *running* RP over SWD: `shared` dumps the sentinel/token/shared variables,
  `counters` reads the command channel's counters (commands answered, dropped, repeated, checksum
  errors, ring overruns, busy/gap/quiet time) without halting, `ring` decodes the commands the ST
  sent from the capture ring (`--mark` / `--since-mark` around a test), on release builds too,
  `heap` reads newlib's allocator, `crash` explains the last reboot and `postmortem` halts for
  backtraces. On debug builds `inject` and `app` drive the firmware through the devhooks mailbox
  (`rp/src/include/devhooks.h`): `swd.py inject 0x0703` sends the Mac's TIME command as if the
  ST had, `swd.py app heap_hold 16` holds 16 KB of heap. They rely on the ELF keeping its
  symbols and on the build ID in flash; both are part of every build.
- Neither this firmware nor the siblings expose picotool's USB reset interface, so `flash.sh`
  flashes through the probe (about 25 s with verification).
- Flash through the probe only with `swd.py program` (or `flash.sh --probe`, which uses it), never
  with OpenOCD's own `program`: halting the cores does not stop the RP2040's DMA, and while the ST
  touches the cartridge the ROM3 capture ring writes bus samples into the RAM the flash write is
  staged in. `swd.py program` stops every PIO state machine and DMA channel first.
- The harnesses turn those tools into PASS/FAIL checks, each with a JSON report in
  `tools/dev/logs/`. `tools_harness.py` checks every tool against a debug build.
  `select_harness.py` presses SELECT over SWD and checks what it does. `power_cycles.py` counts
  cold boots that reach the boot screen. `macdisk_harness.py` injects the Mac's commands through
  the mailbox (no ST needed) and compares what the 68000 would read with host copies of the card's
  images (`--fd0`/`--fd1`/`--hd0`; `--write` rewrites two HD0 sectors and puts them back,
  `--eject UNIT` empties a unit until the next ST boot, `--pram` rewrites `PRAM.BIN` and puts it
  back); it also checks the identity long, the card's state, each unit's image identity, the clock, GET_SETTINGS, every page of LIST and the
  chooser's refusals. `--set` writes the settings flash: a SET that swaps FD1, a FD0 slot stepped
  by a SELECT press and put back on slot 1 by a hello, then every setting put back and the RP restarted. `disktest_harness.py` runs the disk
  self-test of a debug build on the RP itself (`rp/src/devdisktest.c`): it creates its own floppy
  and hard disk images in `SELFTEST/` under the app folder (never touching the card's own), reads
  every sector and writes 1 and 2 sectors through `macdisk_commandCb()` as chandler hands the
  commands over, checks the writes on the card after an eject and after a mount, the read-only
  formats, the refusals, eject and PRAM, then mounts the card's images again and reports the
  command times. Without the probe it runs at boot when `SELFTEST.RUN` is in the app folder and
  writes `SELFTEST.LOG` there. The debug mailbox holds 520 payload
  words (`DEVHOOKS_PAYLOAD_WORDS`, mirrored as `MAILBOX_WORDS` in `swd.py`) so that one full write
  fits. The injected commands share the result slot and the buffer with the ST's own: run the
  harnesses, the self-test and any flash with the ST off or idle at the Jackintosh menu, never
  while the Mac uses its disks (the console log shows READ/WRITE since the last hello). Power-cycle
  the ST sparingly.

The edit-flash-check loop, with `console.py watch` running in another terminal (the harnesses
read its log, never the UART; each exits 0 only when every check passes):

```bash
(cd target/atarist && ./build.sh "$PWD" release 1)   # m68k only; regenerates target_firmware.h
tools/dev/flash.sh debug                              # does NOT rebuild the m68k image: run the line above first
python3 tools/dev/tools_harness.py --build --flash --reset
python3 tools/dev/macdisk_harness.py --hd0 HD0.hda --pram   # a host copy of the card's HD0 image
```

### Planning backlog (`docs/`)
`docs/epics/` holds the local planning notes (`cockpit.sh` regenerates `STATUS.md`; `ITERATIONS.md`
carries the narrative; `DECISIONS.md` the decisions and standing constraints). **The whole `docs/`
tree is gitignored** and lives only on the developer's machine.

- **Never name an epic, story, iteration or task in anything that is committed** — code comments,
  documentation, `CHANGELOG.md`, commit messages, PR descriptions. An identifier like
  `EPIC-03 STORY-01` tells a reader of this repository nothing and cannot be looked up, and a
  commit message cannot be scrubbed afterwards. Write what the code does and why instead: *"the
  heap limit now stops at the start of the cartridge mirror"*, not a pointer to a planning note.
- **Never reference a path under `docs/`** from anything that ships, for the same reason: someone
  who clones the repo has no `docs/`. Inline the information itself.
- Traceability runs the other way: once work lands, record the commit hash in the planning note.
- Before tagging a release, check:
  `git grep -IiE "EPIC-|STORY-|\bepics?\b|\bstor(y|ies)\b|docs/" -- . ':!CLAUDE.md' ':!.gitignore'`
  must come back empty (this section and the `.gitignore` entry are the only places the backlog
  is mentioned).

## Architecture

The firmware is a **two-target build**: m68k assembly that runs on the Atari ST is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — the whole cartridge: header, hello and the boot screen. Lives at `$FA0000` in the ST address space (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …) and its commands; the window layout comes from `inc/sidecart_layout.s`. TOS runs it after GEMDOS init (`CA_INIT` bit 27). `pre_auto` turns a Mega STE's cache off and copies the code into RAM below the screen, where it runs, so an RP reboot or reflash under it does no harm. It says hello and publishes the machine and TOS, then shows the boot screen with GEMDOS calls (`Cconws`, `Cconis`/`Cnecin`): a 30 s countdown on `_hz_200`. X, or the end of the countdown, sends `CMD_BOOSTER` (`$0002`, `EMUL_CMD_BOOSTER` in `rp/src/include/emul.h`) and waits up to 3 s for the RP's `CMD_RESET` in the sentinel, then resets the ST; G puts the Mega STE's cache back and returns to TOS, which boots the GEM desktop; any other key stops the countdown, and then only X or G leave. Every ST boot shows it again. While developing with the ST under TOS, stop the countdown, or the RP leaves for Booster 30 s after every ST boot. The Jackintosh ROM never runs any of it.
- `src/userfw.ld` (the template's name) places `main.s` at offset `0x0000` with the whole 8 KB. Adding an m68k module: give `main.s` less, add a `.text_<name>` section at a fixed offset, mirror the offset with an `equ (ROM4_ADDR + $????)` in `inc/sidecart_layout.s`, and add the `.o` target to `target/atarist/Makefile` (same pattern as `gemdrive.ld` in `md-drives-emulator`). A module linked at an offset must be PC-relative, and ends with the NOP tail (below).
- Built via `stcmd make release` (m68k assembler in Docker); the cartridge image (header + all `.text_*` sections) must fit in 8 KB. A 64 KB padded copy is then converted to `target_firmware.h` for inclusion in the RP build.

### Shared 64 KB cartridge region
The Atari ST sees a 64 KB window at `$FA0000`–`$FAFFFF` (mirrored RP-side at `0x20030000`). This is the **single source of truth** for any cross-target data layout — both sides derive every offset symbolically from constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). **Apps must never hard-code an address inside this region** — always reference the named offset/symbol.

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 8 KB | m68k header + all `.text_*` sections (hard limit) |
| `$FA2000` | `CMD_MAGIC_SENTINEL` | 4 B | the boot screen reads `CMD_RESET` here after asking for Booster |
| `$FA2004` | `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, reserved, then 60 × 4 B indexed shared variables | 256 B | fixed-offset metadata block, ends at `$FA2100` |
| `$FA2100` | (the template's high-res translation table) | 512 B | start of `APP_BUFFERS`; unused here, but `APP_FREE` still starts after it |
| `$FA2300` | `APP_FREE` | ~48 KB | contiguous arena for app buffers: the Mac's status block and sector buffer (`macdisk.h`) |
| `$FAE0C0` | `FRAMEBUFFER` | 8000 B | the template's 320×200 framebuffer; unused here, the offset stays defined in `chandler.h` |

See `programming.md` for the full table and budget rules.

### RP2040 side (`rp/src/`)
- `main.c` — only sets clock/voltage, calls `gconfig_init` (global config) then `aconfig_init` (per-app config), and hands off to `emul_start()`. If config init fails it jumps to the **Booster** app via `reset_jump_to_booster()` to bootstrap. **Don't add features to `main.c`** — put them in `emul.c` or a new module.
- `emul.c` / `emul.h` — the application's main loop and entry point. This is where to add new features. `emul_start()` runs the fixed bring-up order: copy `target_firmware` into `ROM_IN_RAM` → `init_romemul(false)` → `commemul_init()` → `chandler_init()` + `chandler_addCB()` (the boot screen's Booster request, then the disks) and `macdisk_init()` → SELECT → SD and `macdisk_mountAll()` (or, without a usable card, `emul_serveCardError()`) → the RTC settings (`emul_startRtc()`) and the network → main loop. The main loop drains commands, syncs disk writes, polls Wi-Fi and NTP, and serves the debug mailbox; it ends when the boot screen asks for Booster, and then writes `CMD_RESET` into the sentinel, holds it, and jumps to Booster. There is no RP-side UI.
- `romemul.c` / `romemul.pio` — ROM4 **read** engine: a PIO SM latches the 16-bit address, and a chained DMA pair reads that offset out of `ROM_IN_RAM` and pushes it to the PIO TX FIFO. Fully DMA-driven, no IRQ, no CPU. Driven by the `READ_*` / `WRITE_*` GPIOs in `include/constants.h`. Changing these files produces very strange bugs.
- `commemul.c` / `commemul.pio` — ROM3 **command** capture: a PIO SM on `ROM3_GPIO` plus one ring-mode DMA channel continuously records every ROM3 access into an 8,192-sample (16 KB) ring, sized in `commemul.c` from the largest frame the ST can send with its retries. Drained by polling (`commemul_poll`), never by IRQ. A reader that falls a whole ring behind is counted in `commOverruns` and the unread samples dropped; the DMA is re-armed long before its transfer count runs out.
- `chandler.c` / `include/chandler.h` — the polled command dispatcher on top of `commemul` + `tprotocol`, and the RP-side source of truth for the shared-region offsets (`CHANDLER_*`).
- `gconfig.c` / `aconfig.c` — global vs per-app configuration stored in dedicated flash sectors, on top of `settings/` (a key-value store).
- `network.c`, `ntp.c` — Wi-Fi (CYW43, lwIP poll mode) with the credentials Booster stores, and the only thing the network is used for: the time of day from an NTP server, `pool.ntp.org` port 123 unless the app settings say otherwise (`RTC_NTP_HOST`, `RTC_NTP_PORT`, keys 14 and 15 of the chooser's SET_SETTING). With `RTC_ENABLED` "false" (key 13) Wi-Fi is not brought up at all and TIME never turns valid; the three are read at the RP's start (`emul_startRtc()`). `ntp_pollSync()` runs in the main loop's 10 ms network poll and never waits (`md-drives-emulator`'s `rtc.c`, where its request and answer checks come from, waits up to 5 s); it retries every 5 s until a server answers, then counts from the RP's timer. Nothing is downloaded.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO via the bundled `fatfs-sdk`.
- `blink.c`, `select.c`, `reset.c`, `tprotocol.c` — LED Morse status, the activity pulse (`blink_activityPulse()`, the LED on for 25 ms after every card access for the Mac, written after the answer, turned off by `blink_poll()` in the main loop) and the counted flashes of a SELECT step (`blink_startCountSequence()`, which activity pulses do not interrupt), the SELECT button, soft reset/jump-to-booster, command-protocol parser and `TPROTO_*` payload accessors.
- `macdisk.c` / `include/macdisk.h` — the Mac's disks as 512-byte block devices: units 0 and 1 are floppies and unit 2 the hard disk, with app `$07` commands READ `$0700` (up to 32 sectors), WRITE `$0701` (up to 2 sectors, one `send_write_sync`) and EJECT `$0702`, plus TIME `$0703` (UTC seconds since 1904 at `$FA2304` and a VALID flag at `$FA2308`, both 0 until NTP has answered; the Mac applies its time zone) and PRAM_READ `$0704` / PRAM_WRITE `$0705` (the Mac's 256 bytes of parameter RAM, kept in `PRAM.BIN` in the app folder, rewritten in place and synced before the answer), and the disk chooser the Jackintosh ROM draws: LIST `$0706` (a folder's entries, 16 per answer, sorted as md-drives-emulator's browser sorts them, from a listing of up to 256 kept on the heap), GET_SETTINGS `$0707` and SET_SETTING `$0708` (`FOLDER`, FD0's ten slots, FD1 and HD0, full paths of at most 95 bytes in the app settings, the RTC's three settings, and ten ROM options the Jackintosh ROMs keep here without this firmware reading them; a SET is checked, saved to flash at once, about 45 ms, and swaps the unit's image before the answer). `$07` is the first app number no other SidecarTridge firmware uses (`$01` is the legacy ROM emulator's, `$02`–`$06` floppy, RTC, GEMDRIVE, ACSI and pool fix). `macdisk.h` documents the payloads, result codes and window layout, and is the source of truth. At power-on `macdisk_mountAll()` mounts the images the settings name: FD0's slot 1, FD1 and HD0. A unit whose setting is empty stays empty, as a drive with no image does in md-drives-emulator: what the menu shows is what is mounted. `PRAM.BIN` is in the app folder (`FOLDER`, default `/jackintosh`). At every ST boot, once chandler has answered the hello, `macdisk_mountEmpty()` puts FD0 back on slot 1 (ejecting an image a SELECT step put in from another slot), then mounts again each unit that is empty: a Mac's Restart and Shut Down eject its floppies, and its next boot must find them back. Units that hold an image keep it, except FD0 on another slot. To the user FD0 is slot 1 after every boot, and the menu's [A] and SHIFT+A act on the slot GET_SETTINGS answers. A floppy is read-only unless its name ends in `.rw` (md-drives-emulator's rule); the hard disk is read-write. The format is detected from the content: raw (read-write unless the file is read-only, or a floppy not named `.rw`), DiskCopy 4.2, or MacBinary II around DiskCopy 4.2 (both read-only). The status block at `$FA2300` holds the last result and, per unit, flags, sector count, change counter and format, and at `$FA2344` each unit's image identity (the FNV-1a hash of its image's full path, never 0; 0 when empty), written before the flags so a Mac can tell the same image from another after an RP restart; the RP keeps it current. `MACDISK_CARD_OK` is published only after every unit is. Without a usable card nothing else starts (no Wi-Fi, no disks): the RP answers the ST, publishes why at `$FA2340` (`MACDISK_CARD_*`: no card, no file system, no folder), and blinks the LED until it restarts. `$FA230C` holds `MACDISK_ID` (`$4A4B0001`, "JK" and the contract version) from the RP's boot on, card or not: chandler answers every command, handled or not, so the Mac ROM checks it to know this app is the one answering. Sector data is at `$FA2400` (16 KB), byte-swapped so the 68000 reads it in order. Every command is safe to repeat: reads and writes name an absolute sector, and the PRAM and a setting are written whole. Writes are synced `MACDISK_SYNC_DELAY_MS` (3 s) after the first unsynced one. Debug builds log every command with its registers, result and time: the callback records it, and `macdisk_printAnswered()` prints it from the main loop after the answer. The open/read/write, fastseek and deferred-sync code comes from `md-drives-emulator`'s `acsi.c` and `floppy.c`. Not taken: their partition, BPB and TOS hook code, and the multi-chunk write, whose retried last chunk byte-swaps the earlier chunks again (`acsi.c`, the `WRITE_SECTOR` last-chunk path).

### Command path (Atari ST → RP2040)
The cartridge port is read-only, so the m68k sends commands by *reading* from addresses inside ROM3 (`$FB0000`+); the low 16 bits of each address are the data. `tprotocol.c` reassembles that address stream into framed commands (`0xABCD` header, command id, payload size, payload, checksum).

The whole path is **polled, not interrupt-driven**:

```
m68k reads $FB….  →  commemul PIO+DMA ring  →  chandler_loop()  →  tprotocol_parse
                                                      ↓ (complete + checksummed)
                                        registered callbacks, in registration order
                                                      ↓
                        chandler writes the random-token reply into shared memory
```

- Register handlers with `chandler_addCB(cb)` after `chandler_init()`; signature is `void cb(TransmissionProtocol *p, uint16_t *payloadPtr)`, with `payloadPtr` already advanced past the 32-bit random token. Read parameters via the `TPROTO_GET_*` macros and write results back with `memfunc.h` helpers — never with raw pointer arithmetic, because of the endianness swap.
- **`chandler_loop()` must be called from every loop that can block.** The main loop calls it, and so must any long-running wait: `emul.c` installs `emul_pollTick` (`chandler_loop(); emul_serveStBoot(); select_poll();`) via `network_setPollingCallback()` for the multi-second Wi-Fi connect.
- **SELECT is watched on core 0 by `select_poll()`, which never blocks.** Call it wherever `chandler_loop()` is called (the main loop and `emul_pollTick` do). A GPIO edge interrupt records a press made while no loop could poll, and the poll handles it later. A short press restarts the RP, except in a Mac session (the last hello not followed by `$FF00`: the TOS boot screen sends one right after its hello, the Jackintosh ROM never does), where it steps FD0: with the drive empty, the next slot that names an image goes in and the LED flashes once per slot number; with a disk in the drive it does nothing, since swapping it under a mounted volume would corrupt it. A press held for `SELECT_LONG_RESET` (10 s) is a factory reset: `reset_deviceAndEraseFlash()` erases the global settings, and Booster then clears every app's settings. `select_configure()` runs right after the command handler is up, before the SD card and the network. Nothing starts core 1. A SELECT watcher on core 1 ran the long press's flash erase while core 0 executed from the same flash, and both cores faulted until a power cycle. `tools/dev/select_harness.py` checks the presses over SWD.
- The m68k waiter requires the RP to advance `RANDOM_TOKEN_SEED` (a strictly-incrementing counter), not just echo `RANDOM_TOKEN` — echoing alone is indistinguishable from open-bus reads when no cartridge is present. Preserve that invariant in `chandler.c`.

#### What each side may assume about the other
The ST side is `target/atarist/src/inc/sidecart_functions.s` (`send_sync_command_to_sidecart` and its write variant, wrapped by the `send_sync` / `send_write_sync` macros); the RP side is `chandler.c` + `tprotocol.h`. Rules that hold across both, each learned on hardware:

- **The ST's timeout is a spin count, not time, and it is per file.** `COMMAND_TIMEOUT` (`$FFFF`) counts iterations of the token-compare loop, so it shrinks as the CPU gets faster, and each module that includes `inc/sidecart_functions.s` has its own copy.
- **On a Mega STE the cache must be off while the ST talks to the cartridge; the speed does not matter.** Measured on a Mega STE (TOS 2.06) in md-drives-emulator: at 16 MHz without the cache commands work as at 8 MHz; with the cache on they never reach the RP. `main.s` clears bit 0 of `$FFFF8E21` from `pre_auto` (by the `_MCH` cookie) until it hands over, and puts the user's setting back in `boot_gem`; code that talks to the cartridge after that wraps each send in `megaste_cache_off` / `megaste_cache_back`. Never step the machine down to 8 MHz. A reset, the ST's own `jmp` through `$4` included, leaves the machine at 8 MHz with no cache, and stock TOS 2.06 keeps that, so the boot screen runs at 8 MHz; the user's setting arrives later, from XControl.
- **The ST says hello, then publishes the machine and TOS, at every boot.** `main.s` first sends `CMD_ST_HELLO` (`$FF01`, `CHANDLER_ST_HELLO`, no payload) until it is answered, then calls `detect_hw` and `get_tos_version`, which send `CMD_SET_SHARED_VAR` (`$FF00`, `CHANDLER_SET_SHARED_VAR`); `detect_hw` too resends until it is answered. chandler answers both itself, before any callback. The hello makes `chandler_stPresent()` true and `chandler_consumeStBoot()` true once, and `$FF00` makes `chandler_consumeSharedVarSet()` true once. `emul_serveStBoot()` reads both: a hello mounts the empty units again and starts a Mac session, a `$FF00` ends it (only the TOS boot screen sends one). The random token and seed carry on across ST boots on purpose. `$FF00` sets shared variable 0 (the `_MCH` cookie, 0 for an ST) and 1 (ROM TOS version << 16 | GEMDOS `Sversion`). The senders' Mega STE and 68030 checks read variable 0. The Jackintosh ROM sends the hello as well, and nothing after it.
- **What a send keeps**, measured on an ST. `send_sync` keeps d1-d6 and destroys d0, d7, a0-a1; `send_write_sync` keeps d1-d5 and a4 and destroys d0, d6 (its retry count), d7, a0-a1 (both also a2-a3 when `COMMAND_SYNC_USE_DSKBUF` is not 0). Both return with `d0 = 0` and Z set on success, so callers may branch on the flags. The senders leave a0-a1 pointing into the ROM3 command window: a payload read through them is itself sampled by the RP and fails the checksum on every retry. Take what you need into a kept register before the send.
- **A 68030 (TT, Falcon) runs code copied into RAM from a stale instruction cache** unless it is cleared after the copy (CACR's CI bit). This applies to the senders' wait loop when `COMMAND_SYNC_USE_DSKBUF` is not 0, and to anything else copied into RAM and run.
- **End every m68k module with a NOP tail** after `include "inc/sidecart_functions.s"` (`even`, eight `nop`s, a `<module>_end:` label). `firmware.py` strips trailing zeros, the RP copies only `target_firmware_length` words, and the write sender and the 68000's prefetch read past the end of the loop: without the tail those bytes are uninitialised RP RAM.
- **A retry is a new command.** The macros resend up to `CMD_RETRIES_COUNT` times, each with a *fresh* token (the token is the seed read just before sending). The RP may already have executed the first attempt, so a command must be idempotent or carry its own sequence number.
- **Token + seed is a two-phase commit.** The RP stores the token and then the seed; the ST accepts an answer only when the token matches *and* the seed has moved. That also covers absent hardware and a freshly zeroed window.
- **The sentinel is a level, not a queue.** The boot screen reads `CMD_MAGIC_SENTINEL` only after it has asked for Booster, in a loop of up to 3 s; it never reads it inside `send_sync`. A value written once and then replaced can be missed.
- **The two sides reboot independently.** The RP rewrites the 64 KB window when it boots, so anything the ST published at its own cold boot is gone after an RP-only reboot. The boot screen survives that: it runs from its copy in RAM, keeps counting, and the restarted RP answers its Booster request. Code that runs from the cartridge ROM itself does not survive an RP reboot or a reflash, so relocate it to RAM first. Under the Mac ROM the identity long and the unit table read as zero until the RP is back (`macdisk.h`).
- **Answer first.** In `chandler_loop()` the token write is what releases the ST; an LED pulse (on a Pico W that is a CYW43 bus transaction), a blocking `DPRINTF`, or any other slow work goes after it.

The RP drains the ring on every pass of its main loop and never waits: measured on an ST, about 2 ms per command with a 4-byte payload and 3 ms with 1 KB. The parser drops a frame whose payload size is past its buffer, and its 50 ms silence window restarts on every sample. Bringing Wi-Fi up blocks and drains nothing: `network_wifiInit()` took about 0.9 s on a Pico W, and at power-on the ST's boot hello waited that out, which it can, since `main.s` resends it until it is answered. Wi-Fi is polled every 10 ms from the same loop, which is plenty for lwIP's timers. A HardFault inside `cyw43_arch_poll()` right after flashing points at the tools, not the polling: OpenOCD's `rp2040.cfg` puts its work area at `0x20010000`, inside this firmware's RAM, and `verify_image` leaves its CRC routine there; `tools/dev/swd.py` moves the work area to `SCRATCH_X`.

Known defects in this path as the code stands — check before building on it:

- A debug build prints its banner, the flash layout and the settings dumps *before* `emul_start()` makes the cartridge live, so under TOS a power-cycled ST could boot into GEM on a debug build instead of the boot screen. At 921,600 baud the cartridge is live 43-69 ms after the banner, which every ST measured so far waited out; a machine that probes the cartridge sooner is not measured. The Jackintosh ROMs resend their hello until it is answered.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash is sliced into named regions, and code is responsible for not stomping on them:

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch area for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for the Booster app (do not write from this app) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | 192 K | Normal RAM (data, BSS, heap) |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The cartridge window the ST reads at `$FA0000` |
| `SCRATCH_X` / `SCRATCH_Y` | `0x20040000` / `0x20041000` | 4 K each | Core 1 (never started) / core 0 stacks; core 0 has all 4 K of `SCRATCH_Y` |

The heap's limit, `__StackLimit`, is the end of `RAM`, where the cartridge window starts: the SDK's
`sbrk` stops there, and with `PICO_MALLOC_PANIC=0` (set before `pico_sdk_init()` in
`rp/src/CMakeLists.txt`, the only place it reaches the SDK's `malloc.c`) malloc returns NULL instead
of panicking. Every allocation in `rp/src` checks for NULL.
`PICO_HEAP_SIZE` (32 KB) is the heap the link guarantees: a build whose static data leaves less
fails with ``region `RAM' overflowed``. A release build of this app: flash 493,484 bytes, static RAM
71,152, heap 125,456; LIST's listing takes 26 KB of the heap from the first LIST on.

Core 0's stack is all of `SCRATCH_Y` (`PICO_STACK_SIZE` 4 KB) with an MPU guard on its bottom 32
bytes (`PICO_USE_STACK_GUARDS`): an overflow is a HardFault, which `fatfs-sdk`'s crash handler
records in `crash_info_ram` and resets. Keep large buffers off the stack (`macdisk.c` keeps its
`FILINFO`, paths and buffers static): `tools/dev/stackdepth.py` on a `tools/dev/measure_builds.sh`
build lists every frame.

The build assumes Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`). The PIO bus emulation runs hot — Core 0 also overclocks to 225 MHz at `VREG_VOLTAGE_1_10`.

### App identity
`CURRENT_APP_UUID_KEY` (set from the `APP_UUID_KEY` env var at CMake time, with a placeholder default) is the app's UUID4. It must match the `uuid` field in `desc/app.json` and is used as the key into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → app jumps to Booster.

## Publishing

A successful build leaves three files in `dist/`: `<APP_UUID>-<VERSION>.uf2` (the firmware users install), `<APP_UUID>.json` (the app descriptor) and `rp.uf2.md5sum` (a byproduct; its hash is already inside the descriptor).

The descriptor is generated, not hand-written. Edit `desc/app.json`: fill in `name`, `description`, `image`, `tags`, `devices` and `binary`, and **leave `<APP_UUID>`, `<APP_VERSION>` and `<BINARY_MD5_HASH>` as placeholders** because `build.sh` substitutes them on every run. Write the `binary` URL with those placeholders too, so it tracks the version across releases. `build.sh` aborts if `desc/app.json` is missing.

Getting an app into the public catalogue that Booster downloads is **one pull request** against [`sidecartridge-microfirmwares-store`](https://github.com/sidecartridge/sidecartridge-microfirmwares-store), adding an origin that points at an `apps.json` you host yourself. The store keeps a pointer, not a copy, so later releases need no further pull request. Walkthrough: <https://md-store.sidecartridge.com/build/atari-st/11-get-listed.html>

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — they are git submodules pinned to specific upstream revisions, and the build re-pins them on every run. To change FatFs configuration, edit `rp/src/ff/ffconf.h` (project-owned override); the include path is set up so this file wins over the submodule's default.
- Don't touch `main.c` for feature work — start in `emul.c`.
- Match the existing C style (clang-format config in `.clang-format`, clang-tidy in `.clang-tidy`). VS Code runs both. The command-line build runs neither, so a clean build is not a clang-tidy pass.

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan with a verification check per step.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
