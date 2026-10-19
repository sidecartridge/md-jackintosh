# Changelog

## v1.0.0 (unreleased) - md-jackintosh

The first version of md-jackintosh, built from the template v1.3.0. It
serves the disks, the clock and the parameter RAM of the Jackintosh ROMs,
which turn an Atari ST or STE into a Macintosh Plus at power-on with no TOS
and talk to the cartridge themselves. Under TOS the cartridge only shows a
boot screen. Seen on an STE: System 6.0.8 boots from a floppy image and from
a hard disk image, Word 4.0a runs from the second floppy and saves to it,
and a Restart comes back to the Finder.

### Added

- **The Mac's disks** (`rp/src/macdisk.c`, `rp/src/include/macdisk.h`):
  two floppies and a hard disk as 512-byte block devices, app `$07`:
  READ `$0700` (up to 32 sectors), WRITE `$0701` (up to 2 sectors, one
  `send_write_sync`) and EJECT `$0702`. At power-on the images the
  settings name are mounted (see the disk chooser below), and a unit with
  none stays empty: raw images read-write (a floppy only when its name ends
  in `.rw`, as in md-drives-emulator), DiskCopy 4.2 and MacBinary II
  DiskCopy 4.2 read-only. The
  status block at `$FA2300` holds the last result, an identity long
  (`$4A4B0001`) and, per unit, flags, sectors, a change counter and the
  format; sector data is at `$FA2400`. Writes are synced 3 s after the
  first unsynced one. A unit an EJECT emptied, as a Mac's Restart and
  Shut Down do, is mounted again at the ST's next boot. `macdisk.h` is the
  contract with the ROMs.
- **The disk chooser** the ROMs draw, as md-drives-emulator's menu:
  LIST `$0706` lists a folder of the card, 16 entries at a time (".."
  first below the app folder, then folders, then files, sorted ignoring
  case, dot names hidden; or from "/", for choosing the app folder);
  GET_SETTINGS `$0707` and SET_SETTING `$0708` read and write the app
  folder, FD0's ten slots, FD1 and HD0 in the app settings. A SET is
  checked (inside the app folder, a file for an image, a folder for the
  app folder), saved at once, and swaps the unit's image before the
  answer. In a Mac session a short SELECT press puts FD0's next slot in
  the drive if the drive is empty, and the LED flashes once per slot
  number; with a disk in the drive it does nothing. Outside one it still
  restarts the RP.
- **The time of day** (`rp/src/ntp.c`): an NTP client that never blocks
  the command channel; TIME `$0703` gives UTC seconds since 1904. The
  chooser's settings turn it on or off and name the server and port
  (md-drives-emulator's `RTC_ENABLED`, `RTC_NTP_HOST`, `RTC_NTP_PORT`;
  `pool.ntp.org`, 123 by default), from the RP's next start; with it off
  the RP does not bring Wi-Fi up at all.
- **The parameter RAM**: PRAM_READ `$0704` and PRAM_WRITE `$0705` keep the
  Mac's 256 bytes in `PRAM.BIN` in the app folder.
- **The TOS boot screen** (`target/atarist/src/main.s`), in GEMDOS calls:
  X goes to Booster, G to the GEM desktop, after 30 s Booster starts on
  its own, and any other key stops the countdown. The version comes from
  `version.txt`.
- **No usable microSD card, no service**: without a card, with no FAT or
  exFAT file system on it, or when the app folder cannot be created,
  nothing else starts (no Wi-Fi, no disks). The LED blinks every 700 ms
  until the RP restarts, the ST is still answered, and the reason is in
  the status block at `$FA2340` (0 OK, 1 no card, 2 no file system, 3 no
  folder, 4 not tried yet) for the ROMs to show.
- **The activity LED**: off when idle, on for 25 ms after every access to
  the card for the Mac, as in md-drives-emulator.
- **The disk self-test** (`rp/src/devdisktest.c`, debug builds): on the
  device, it creates its own floppy and hard disk images on the card,
  reads and writes them through the Mac's commands and checks every byte;
  started over SWD (`tools/dev/disktest_harness.py`) or by `SELFTEST.RUN`
  in the app folder. `tools/dev/macdisk_harness.py` checks the commands
  against host copies of the card's images. Debug builds log every disk
  command after its answer.

### Changed

- The app folder is `/jackintosh` (the `FOLDER` default), created if it
  is missing. The app settings version is 2 while developing, so settings
  saved by the template build under the shared development UUID are not
  loaded.
- The cartridge image is `main.s` alone, with the whole 8 KB.
- The debug mailbox holds 520 payload words, one full disk write.
- A missing card is told from a card with no file system: with card detect
  off it fails the mount as not ready (`sdcard.c`).
- The app descriptor and its icon (`desc/app.json`, `icon.png`): Jackintosh
  Multidrive Emulation, for the ST, STE, Mega ST and Mega STE.

### Removed

- The RP-drawn setup menu and everything behind it: `display.c`,
  `display_term.c`, `term.c`, `u8g2/`, and the user firmware module
  (`userfw.s`, `CMD_START`).
- The download stack and the HTTPS build profile (`download.c`, `httpc/`,
  `devdownload.c`, `md5/`, mbedTLS, `APP_DOWNLOAD_HTTPS`): the network
  is used for NTP only.
- The dev tools that drove the menu: `st_harness.py`, `sttest.s`,
  `download_harness.py`, and `swd.py`'s `screen`, `text` and `key`.

## v1.3.0 (2026-09-29) - release

A reliability release. The command path between the ST and the RP now
answers at once and survives bad input, the ST says who it is at every
boot, a release build is a release build, the memory has limits that
hold, downloads can use HTTPS, and the checks this release was verified
with ship in `tools/dev/`. Measured on an ST (TOS 1.04) and a Mega STE
(TOS 2.06).

### Before you update an app built from v1.2.x

- **Release builds print nothing.** Up to v1.2.1 `DEBUG_MODE=0` still
  compiled `_DEBUG=1`, so every release UF2 was a debug build. Build
  with `debug` to get the serial console.
- **The debug console runs at 921,600 baud** (it was 115,200). Set your
  serial terminal to match.
- **Both build types are CMake `Release` (-O3)** (they were
  `MinSizeRel`); `debug` only adds `DEBUG_MODE=1`. `RP_CMAKE_BUILD_TYPE`
  builds another CMake type, with a warning. The build type must be
  `release` or `debug`, in any case; anything else stops the build.
- **`CMD_SET_SHARED_VAR` is `$FF00`** (it was 1, the terminal's
  keystroke command). The RP answers it, and the new `CMD_ST_HELLO`
  (`$FF01`), itself, before any callback.
- **SELECT's blocking helpers are gone**: `select_coreWaitPush()`,
  `select_coreWaitPushDisable()`, `select_waitPush()` and
  `select_checkPushReset()`; the first ran a watcher on core 1. Call
  `select_poll()` wherever you call `chandler_loop()`, and register the
  actions with `select_setResetCallback()` /
  `select_setLongResetCallback()`.
- **Core 0's stack is 4 KB**, all of `SCRATCH_Y` (it was 2 KB), with a
  guard at its bottom: an overflow is a HardFault instead of silent
  damage. Keep large buffers off the stack.
- **`malloc` returns NULL** instead of panicking, and the heap stops at
  the end of RAM instead of running into the cartridge window. Check
  every allocation.
- **User firmware on a Mega STE** turns the cache off around each send
  (`megaste_cache_off` / `megaste_cache_back`): with the cache on,
  commands never reach the RP. Never step the machine down to 8 MHz.
- **Every m68k module ends with a NOP tail** after
  `include "inc/sidecart_functions.s"`: `even`, eight `nop`s and a
  `<module>_end:` label.
- **Settings**: never remove an entry from an app's defaults table; add
  new ones at the end. The loader reads as many stored entries as there
  are defaults.
- **Downloads**: an `https://` URL in the default build fails with
  `DOWNLOAD_HTTPSNOTBUILT_ERROR` instead of being fetched over plain
  HTTP. Build with `APP_DOWNLOAD_HTTPS=1` to fetch it.

### Command path (ST to RP)

- **Answers at once.** The main loop drains the ROM3 ring on every pass
  and never waits; it used to wait up to 100 ms for Wi-Fi work first.
  Measured on an ST: 25.6 ms to 2.1 ms per command with a 4-byte
  payload, 324 ms to 2.5-3 ms with 1 KB. The parser's silence window is
  50 ms and restarts on every sample, so a frame that spans two drains
  is no longer thrown away.
- **An oversize frame is dropped.** A payload size past the 2 KB buffer
  used to overwrite whatever followed it (on hardware, the Wi-Fi
  driver's state, and the RP hard-faulted). The ST gets no answer, and
  the next command is answered.
- **The ST says hello at every boot.** `main.s` sends `CMD_ST_HELLO`
  until it is answered, then publishes the machine (`_MCH` cookie,
  shared variable 0) and the TOS version (variable 1).
  `chandler_stPresent()` and `chandler_consumeStBoot()` tell the app;
  `[F]irmware` waits for the hello, and an ST reset clears what was
  typed before it.
- **The senders return `d0 = 0` with Z set** on success (since the seed
  check they returned with Z clear). What each send keeps is measured
  and documented: `send_sync` keeps d1-d6, `send_write_sync` d1-d5 and
  a4.
- **The ROM3 ring is 16 KB** (8,192 samples), sized from the largest
  send with its retries. A reader that falls a whole ring behind is
  counted (`commemul_getOverruns()`) and resynchronises; the capture is
  re-armed before its transfer count runs out, so it no longer stops
  after 2^32 samples.
- **Counters.** chandler counts commands answered, dropped, repeated and
  with a bad checksum, and times them. `tools/dev/swd.py counters` reads
  them from a running RP, on a release build too.
- The exit command is held until the ST's menu loop has seen it, and
  commands are answered meanwhile.
- The cartridge image is copied into the window exactly: the last word of
  an odd-length image was dropped, and the rest of the window held an
  earlier app's code. The window is cleared first.
- Every interrupt is masked and cleared before jumping to Booster, so an
  interrupt at the jump can no longer lock the core up.

### m68k framework

- `inc/sidecart_layout.s` holds the window's layout and the command
  channel's constants, shared by `main.s` and `userfw.s`. User firmware
  includes it with the macros and the senders, so it can name the window
  and send commands. Its code must be PC-relative.
- Mega STE: `main.s` turns only the cache off from the start of the
  cartridge until it hands over, and gives back the user's setting. On a
  Mega STE with TOS 2.06 the setup menu and the user firmware at 8 MHz,
  and the command tests at 16 MHz with and without the cache, all pass.
- From md-drives-emulator: a 68030's instruction cache is cleared after
  the senders copy their wait loop (when `COMMAND_SYNC_USE_DSKBUF` is not
  0), and `inc/tos.s` gains `Mfpint` and `Flopver`.
- `programming.md` covers user firmware, what the ST and the RP may
  assume about each other, and the rules for resident trap hooks.

### Setup menu and SELECT

- The menu fits in 40 by 24 and no longer scrolls on every refresh. It
  shows the machine and TOS the ST reported, redrawn when the ST
  publishes them.
- SELECT is watched on core 0 by `select_poll()`, which never blocks; a
  GPIO edge interrupt catches a press made while nothing polls. A short
  press restarts the RP. A press held 10 s is a factory reset: the global
  settings are erased, and Booster clears every app's settings.
- After `[F]irmware` the sentinel keeps the start command, so every ST
  boot runs the user firmware again, as an app's emulation mode would. A
  short SELECT press brings the setup menu back.

### Memory

- The heap stops at the end of RAM, before the cartridge window. With
  `PICO_MALLOC_PANIC=0` a failed allocation returns NULL.
- `PICO_HEAP_SIZE` (32 KB) is the heap the link guarantees: a build
  whose static data leaves less fails to link. Measured peak: 13.1 KB.
- The large buffers are off the stack. The deepest measured use of core
  0's stack went from 7,360 bytes (past its 2 KB and through core 1's
  stack) to 1,696.

### Downloads

- `APP_DOWNLOAD_HTTPS=1` builds HTTP and HTTPS, chosen per URL, with
  mbedTLS and larger lwIP buffers (about 31 KB of static RAM, and an 8 KB
  stack for core 0). The default builds HTTP only, as before. The
  server's certificate is not verified: there is no CA bundle and no
  wall clock.
- `download_poll()` never waits: the body is written 4 KB per call, so
  the ST's commands keep flowing during a download (10 ms each, 17 ms
  over HTTPS, none lost).
- Only a 2xx response reaches the file. Any failure deletes the
  temporary file and says why (`download_getError()`,
  `download_getHttpStatus()`). Redirects are followed, up to 5, and a
  failed hop is retried twice. New error codes are appended, so the
  existing values keep their numbers.
- A URL longer than `DOWNLOAD_URL_SIZE` is refused, never cut. Explicit
  ports work, and a query string no longer ends up in the saved name.

### SD card, settings and network

- SD: the card and SPI lookups are bounded by their own tables, and
  `fatfs-sdk` is pinned to v3.6.2, which releases its locks when a read
  fails. A pulled card now fails at once instead of hanging.
- `FF_FS_TINY` is 1 for both build types.
- The Wi-Fi password is masked in every log.
- Settings: the flash size, offset and number of defaults are checked at
  run time; the `assert()`s they replace were compiled out of every
  build.
- A static IP is validated before use; a bad one leaves DHCP running and
  says why. Wi-Fi power saving stays off after every reconnect. The
  unused scan path is gone. The global defaults match Booster v2.4.1.

### Build and CI

- The build scripts stop at the first failure and leave nothing in
  `dist/`. `RELEASE_DATE` makes two builds of one commit byte-identical.
- Every build carries a build ID (`<sha7>`, `-dirty.<diff7>`, `+https`,
  `+debug`) in flash, and `rp.elf` keeps its symbols.
- mbedTLS is compiled `-Os` whatever the build type: at -O3 every TLS
  handshake failed (`library/gcm.c`).
- CI builds release and debug, HTTP and HTTPS, on every pull request,
  with ARM GNU Toolchain 14.2 and atarist-toolkit-docker v1.2.1. It used
  to be green while every m68k build failed.

### Developer tools (`tools/dev/`)

With a Raspberry Pi Debug Probe attached:

- `console.py` captures the debug console. `flash.sh` builds
  incrementally, flashes and verifies. `swd.py` reads a running RP: the
  ST's screen, the terminal, the shared variables, the heap, the
  counters, the commands in the capture ring, the last crash. On debug
  builds it also types keys and drives the firmware through a mailbox.
- Harnesses with PASS/FAIL and a JSON report: `tools_harness.py` (the
  tools), `st_harness.py` (the command path from the ST's side, with
  `--mste` for a Mega STE's speed and cache), `select_harness.py`,
  `download_harness.py` and `power_cycles.py`.
- `measure_builds.sh` and `stackdepth.py` report flash, RAM, heap and
  worst-case stack depth.

---

## v1.2.1 (2026-05-20) - release

### Sync ack: no false positives on missing hardware

The m68k `send_sync` / `send_sync_write` waiters previously ack'd a
command as soon as `RANDOM_TOKEN == d2` (the token the m68k sent).
That condition also holds when the cartridge is unplugged or the
microfirmware is not running, because the bus returns the same
open-bus / uninitialised value at both `RANDOM_TOKEN_ADDR` and
`RANDOM_TOKEN_SEED_ADDR` — apps would proceed as if the device had
replied.

The waiter now additionally requires `RANDOM_TOKEN_SEED_ADDR` to have
moved away from the value the m68k loaded at request time. The RP
side already writes a strictly-incrementing `incrementalCmdCount` to
the seed slot on every response, so a real reply always advances the
seed; absent hardware leaves it equal to `d2` and the loop runs to
timeout instead.

RP-side `chandler.c` gains a comment documenting the
SEED-must-advance invariant that the m68k waiter depends on.

Thanks to @neilrackett (PR #5) for the fix.

---

## v1.2.0 (2026-04-28) - release

### Shared 64 KB region rearranged

Single source-of-truth layout for the region mirrored at m68k
`$FA0000` / RP `0x20030000`. Cartridge code lives in the first 8 KB,
metadata block sits just above it, and the framebuffer moves to the
top so app data fills one contiguous 48 KB arena.

| m68k addr | Region                                       |
| --------- | -------------------------------------------- |
| `$FA0000` | CARTRIDGE (max 8 KB, build.sh-enforced)      |
| `$FA2000` | sentinel + random token + 60 shared vars     |
| `$FA2100` | TRANSTABLE (512 B high-res mask table)       |
| `$FA2300` | APP_FREE (~48 KB)                            |
| `$FAE0C0` | FRAMEBUFFER (8000 B)                         |

Reference offsets via the constants in `rp/src/include/chandler.h`
and `target/atarist/src/main.s`. Apps that hard-coded the old
addresses (`$FA8000` framebuffer, `$FAF000` random token) must
migrate.

### User firmware module

New per-module split via `target/atarist/src/userfw.ld`:
2 KB for `main.s` + 6 KB for the new `userfw.s` (entry at
`USERFW = $FA0800`). Pattern mirrors md-drives-emulator's
`gemdrive.ld`.

Launch path: RP terminal menu `[F]irmware` → `CMD_START = 4` on the
cartridge sentinel → `rom_function: jmp USERFW`. Default `userfw.s`
is a Cconws demo that clears the screen and prints
`Example firmware load...`.

---

## v1.1.0 (2026-04-27) - release

Architectural port of the framework improvements introduced in
md-drives-emulator. Apps derived from previous versions of this template
will need to migrate (see "Breaking changes" below).

### Memory layout
- RAM grown from 128 KB to 192 KB.
- ROM_IN_RAM reduced from 128 KB @ `0x20020000` to 64 KB @ `0x20030000`.
- Result: 64 KB of additional general-purpose RAM available to apps.

### ROM4 read engine (cartridge data path)
- Single-bank 64 KB ROM (ROM_BANKS 2 → 1). ROM3 is no longer a data
  bank; it is now used exclusively as the command channel.
- PIO program rewritten: waits directly on `ROM4_GPIO`, captures 16
  bits of address, two-channel chained DMA serves reads with no CPU
  or IRQ involvement.
- `ROMEMUL_BUS_BITS` 17 → 16; `FLASH_ROM3_LOAD_OFFSET` removed.

### ROM3 command channel — new (`commemul`)
- Dedicated PIO state machine on `ROM3_GPIO` captures every ROM3
  access into a 32 KB ring buffer via DMA in ring mode (no IRQ).
- `commemul_poll(callback)` drains the ring lock-free using
  `dma_hw->ch[ch].transfer_count` to derive the producer index.

### Command dispatcher — new (`chandler`)
- Polled command parser/dispatcher. `chandler_loop()` calls
  `commemul_poll`, parses via `tprotocol_parse`, and dispatches each
  command to a registered callback list.
- Apps register handlers with `chandler_addCB(callback)`.
- Replaces the previous `DMA_IRQ_1`-driven snoop in `term.c`.

### Terminal
- Removed `term_dma_irq_handler_lookup`. Terminal commands are now
  delivered through `term_command_cb`, which is registered with
  `chandler_addCB` during emulation startup.

### Orchestration (emul.c)
- Boot now calls
  `init_romemul(false); commemul_init(); chandler_init();
  chandler_addCB(term_command_cb);`.
- Main loop drains commands via `chandler_loop()` before `term_loop()`.
- WiFi polling callback drains both, so commands sent during the
  multi-second WiFi connect window are not dropped.

### m68k framework fixes
- `inc/tos.s`: fix `endmv` → `endm` typo in the `pchar2` macro.
- `inc/sidecart_functions.s`: rename transient `d0` → `d4` in the
  `_write_to_sidecart_*` loops. `d0` is the sync command reply
  register; reusing it mid-write clobbered the reply slot for any
  app that issued payload writes.
- New `COMMAND_WRITE_TIMEOUT` (defaults to `COMMAND_TIMEOUT`) used by
  `_start_sync_write_code_in_stack`, so apps can extend the write
  timeout for large payloads without affecting read-command timing.

### Breaking changes for downstream apps
- `init_romemul(IRQInterceptionCallback, IRQInterceptionCallback,
  bool)` → `init_romemul(bool)`.
- Removed: `dma_setResponseCB`, `romemul_getLookupDataRomDmaChannel`,
  `dma_irqHandlerLookup`, `dma_irqHandlerAddress`, the
  `IRQInterceptionCallback` typedef.
- `tprotocol`: `TransmissionProtocol.payload` is now `uint16_t[/2]`
  (was `unsigned char[]`). Static parser state is now extern; the
  template provides `tprotocol.c` defining the externs.
- `term.h`: `ADDRESS_HIGH_BIT` and the `ROM3_GPIO` define removed.

### Build
- Added `chandler.c`, `commemul.c` to `rp/src/CMakeLists.txt` sources.
- Added `pico_generate_pio_header` for `commemul.pio`.

---

## v0.0.3 (2025-07-01) - release
- First version

---
