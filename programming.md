# Programming md-jackintosh

This guide is for developers of md-jackintosh, the SidecarTridge Multi-device microfirmware that
serves the disks, the clock and the parameter RAM of the Jackintosh ROMs. It covers what this
repository adds to the
[Multi-device microfirmware template](https://github.com/sidecartridge/md-microfirmware-template):
how the parts fit, the modules, the memory, the cartridge window and the commands. For what
every microfirmware shares (the development environment, the template's modules, the command
protocol in general) see the
[official programming documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

`CLAUDE.md` has the build gotchas and the rules learned on hardware, `AGENTS.md` copy-pasteable
commands and a symptom → fix table, and `tools/dev/README.md` the debug tools.

## How the parts fit

| Part | Where | What it does |
| --- | --- | --- |
| The Jackintosh ROMs | [md-jackintosh-roms](https://github.com/sidecartridge/md-jackintosh-roms), built at <https://jackintosh.sidecartridge.com>, in the TOS sockets: served by the [SidecarTridge TOS Emulator](https://sidecartridge.com/products/sidecartridge-tos-atari-st/), or burnt into EPROMs or EEPROMs | Boot the ST or STE as a Mac Plus, with no TOS. Draw the disk menu. Send the commands below over the cartridge port, starting with `CMD_ST_HELLO`. |
| The RP firmware | `rp/src` | Serves the Mac's disk images from the microSD card, the time of day from NTP and the parameter RAM; keeps the menu's choices in the app settings. |
| The cartridge code | `target/atarist/src/main.s` | Runs only under TOS, that is, without the Jackintosh ROMs, when the app has nothing to serve: a page that says the app works only with them, and sends the user to Booster or the GEM desktop. The Jackintosh ROMs never run it. |

The ROMs and this firmware meet at three things, which together are the contract between the two
repositories: the command protocol, the cartridge window layout in `rp/src/include/chandler.h`,
and `rp/src/include/macdisk.h`. Change any of them only after agreeing it with md-jackintosh-roms.
`macdisk.h` is the source of truth for the commands, their payloads, the results and the window
layout; this guide summarises it.

## Development environment

Follow the [official documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/)
for the Pico SDK, the ARM GNU Toolchain 14.2 (`PICO_TOOLCHAIN_PATH`) and
[atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker) (`stcmd`). The
SDK paths (`PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`) default to the submodules in
this repository.

On top of that, md-jackintosh needs:

- **A Raspberry Pi Debug Probe** on the Multi-device: the RP side cannot run off-hardware, and
  every tool in `tools/dev/` works through it.
- **An Atari ST or STE with the Jackintosh ROMs** in its TOS sockets, and Mac disk images on the
  microSD card. The [SidecarTridge TOS Emulator](https://sidecartridge.com/products/sidecartridge-tos-atari-st/)
  makes this practical: it serves ROM images from its own storage, so a new ROM build, or TOS
  again, is a load away rather than a chip to burn and swap. Hatari can show the TOS boot screen
  only: it has no Multi-device.
- **Booster** on the Multi-device, from its debug build, with the `DEV APP` launched once, so the
  development UUID `44444444-4444-4444-8444-444444444444` has a settings sector.

## Building

```bash
# ./build.sh <board_type> <build_type> <app_uuid>
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
./build.sh pico_w debug 44444444-4444-4444-8444-444444444444
```

`build.sh` builds the cartridge code first (`target/atarist/build.sh`, at most 8 KB), embeds it
in the RP firmware as `rp/src/include/target_firmware.h`, builds the RP firmware, and leaves in
`dist/` the UF2, the app descriptor (from `desc/app.json`) and an MD5. Both build types are CMake
`Release`: `debug` only turns on the debug console (921,600 baud on the probe's UART), the debug
mailbox and the disk self-test.

For the edit-flash-check loop, `tools/dev/flash.sh debug --probe` builds out of tree in seconds,
flashes through the probe and checks the result. It does not rebuild the cartridge code: run
`(cd target/atarist && ./build.sh "$PWD" release 1)` first when `main.s` changed.

## The RP2040 side

### `emul.c`: the start and the main loop

`emul_start()` runs, in this order:

1. Copies the cartridge image into `ROM_IN_RAM` and brings up the ROM4 read engine
   (`init_romemul`), the ROM3 command ring (`commemul_init`) and the command dispatcher
   (`chandler_init`), with two callbacks: `emul_bootScreenCb` (the boot screen's request for
   Booster) and `macdisk_commandCb` (everything the Jackintosh ROMs send). `macdisk_init()`
   publishes the identity before the first answer.
2. Configures SELECT (`select_configure`): a short press goes to `emul_selectShortPress()`, a
   long one to `reset_deviceAndEraseFlash()`.
3. Mounts the microSD card and creates the app folder (`FOLDER`, default `/jackintosh`). Without
   a usable card nothing else starts: `macdisk_setCardStatus()` publishes why, and
   `emul_serveCardError()` keeps answering the ST and blinks the LED every 700 ms until the RP
   restarts. With a card, `macdisk_mountAll()` mounts the chosen images, and a debug build runs
   the disk self-test if `SELFTEST.RUN` is in the app folder.
4. Reads the RTC settings (`emul_startRtc()`): with `RTC_ENABLED` "false" the network is never
   brought up; otherwise `ntp_setServer()` takes `RTC_NTP_HOST` and `RTC_NTP_PORT`, and Wi-Fi
   connects in STA mode with the credentials Booster stores. The connect blocks for seconds, so
   `emul_pollTick` (`chandler_loop`, the ST-boot work, `select_poll`) is installed as the network's
   polling callback, and the ST is answered throughout.
5. The main loop, which never waits: the debug mailbox, `select_poll()`, `chandler_loop()`, the
   ST-boot work (`macdisk_mountEmpty()` after each hello), a FD0 step a SELECT press asked for, the
   LED, the debug command log, `macdisk_syncPending()`, and every 10 ms the network poll and the
   NTP client (`ntp_pollSync()`).
6. When the boot screen asks for Booster: `CMD_RESET` in the sentinel, held while the ST sees it,
   then the jump to Booster.

A **Mac session** is an ST boot whose hello no `CMD_SET_SHARED_VAR` follows: the TOS boot screen
sends one right after its hello, the Jackintosh ROMs never do. In a Mac session a short SELECT
press steps FD0's slots instead of restarting the RP.

### `macdisk.c`: the Mac's disks

Three units of 512-byte sectors: FD0 (internal floppy), FD1 (external floppy), HD0 (hard disk).

- **What is mounted.** The app settings name the images as full paths: FD0's ten slots (`FD0`,
  `FD0_2` to `FD0_10`), `FD1` and `HD0`. At power-on FD0 holds slot 1; a unit whose setting is
  empty stays empty. At every ST boot, `macdisk_mountEmpty()` mounts again each unit an EJECT
  emptied (a Mac's Restart and Shut Down eject its floppies), FD0 from its current slot.
- **Formats**, told from the content: raw, DiskCopy 4.2 (data from `$54`), MacBinary II around a
  DiskCopy 4.2 image. DiskCopy images are read-only: a write would leave the header's checksum
  stale, and the tags are not served.
- **The `.rw` rule**, md-drives-emulator's: a floppy is opened read-only unless its name ends in
  `.rw`, in any case. The hard disk is read-write unless the file is read-only on the card.
- **Reads and writes.** READ takes up to 32 sectors, WRITE up to 2 (one `send_write_sync`). A
  fastseek cluster map makes the seeks of a fragmented image cheap. Writes are synced
  `MACDISK_SYNC_DELAY_MS` (3 s) after the first unsynced one, so a burst pays the directory update
  once.
- **The chooser.** LIST lists a folder for the menu, 16 entries per answer, from a listing of up
  to 256 entries (26 KB, allocated from the heap at the first LIST and kept) that is made again
  when the first entry is 0, or the folder or d4 changes. GET_SETTINGS and SET_SETTING read and write
  the settings; a SET is checked first (an absolute path inside `FOLDER`, no `..`, no backslash),
  writes the flash only when the value changes (about 45 ms), and swaps the unit's image before
  the answer.
- **The FD0 step.** `macdisk_stepFd0()`, for SELECT: with FD0 empty, the next slot that names an
  image becomes current and goes in; with a disk in FD0 it does nothing.
- **Debug builds** log every command with its registers, result and time; the callback only
  records it and `macdisk_printAnswered()` prints it after the answer.

The open, read, write, fastseek and deferred-sync code comes from md-drives-emulator's `acsi.c`
and `floppy.c`. Its multi-chunk write is left out: its retried last chunk byte-swaps the earlier
chunks again.

`devdisktest.c` (debug builds) is the disk self-test: it creates its own floppy and hard disk
images in `SELFTEST/` under the app folder, mounts them by name (`macdisk_mountByName()`), checks
every sector through `macdisk_commandCb()`, and puts the card's own images back.

### `aconfig.c`: the app settings

| Key | Default | Meaning |
| --- | --- | --- |
| `FOLDER` | `/jackintosh` | The top of the menu's browser; `PRAM.BIN` lives here |
| `MODE` | `255` | The template's app mode; saved before the jump to Booster |
| `FD0`, `FD0_2` … `FD0_10` | `""` | FD0's slots 1 to 10 |
| `FD1`, `HD0` | `""` | FD1's and HD0's images |
| `RTC_ENABLED` | `true` | The time of day from NTP, and the Wi-Fi with it |
| `RTC_NTP_HOST` | `pool.ntp.org` | The NTP server |
| `RTC_NTP_PORT` | `123` | Its port |
| `ROM_OPTION_0` … `ROM_OPTION_9` | `""` | The Jackintosh ROMs' options, kept for them and never read here. Today the ROMs use 0 for the menu's countdown, 1 for the Mac's RAM, 2 for the right mouse button, 3 for the screen, 4 for the keyboard layout, 5 for the mouse's tracking speed and 6 for a Mega STE's speed and cache; "" means their default |

Never remove an entry from `defaultEntries` and add new ones only at the end: the loader reads
only as many stored entries as there are defaults. The app's 4 KB settings sector holds 32 entries
of 127 bytes, one of them the version: 31 settings, of which these take 27. `settings_init()`
refuses a default table that does not fit, and the app then drops to Booster. `ACONFIG_VERSION_NUMBER` is 2 while
developing, so settings saved under the shared development UUID by the template build are not
loaded; it goes back to 1 with the app's own UUID, before the release.

### `ntp.c` and `network.c`: the time of day

The network exists only for NTP: nothing is downloaded. `ntp_pollSync()` is a state machine
stepped from the main loop: nothing happens until Wi-Fi has an address, then it looks the server
up and asks, retrying every 5 s until one answer arrives, and from then on counts from the RP's
timer. `ntp_getUnixTime()` gives UTC seconds since 1970. The request and the answer's checks come
from md-drives-emulator's `rtc.c`, whose query waits for the answer instead. The Mac applies its
own time zone, kept in its parameter RAM.

### `blink.c`, `select.c`, `sdcard.c`

- **The LED** (the Pico W's, through the Wi-Fi chip, so it is written only after the answer):
  `blink_activityPulse()` lights it for 25 ms after a card access for the Mac;
  `blink_startCountSequence()` flashes it N times, 200 ms on and 200 ms off, for FD0's slot N,
  and activity pulses do not interrupt it; `blink_toogle()` blinks it every 700 ms without a
  usable card.
- **SELECT** is watched on core 0 by `select_poll()`, which never blocks: a short press runs
  `emul_selectShortPress()` on release, a press held 10 s is a factory reset.
- **The card**: no card, or one that does not answer, is `MACDISK_CARD_NONE` (with card detect
  off it fails the mount as not ready); no FAT or exFAT file system is `MACDISK_CARD_UNREADABLE`;
  an app folder that cannot be created is `MACDISK_CARD_NO_FOLDER`.

### The template's modules

`romemul` (the ROM4 read engine), `commemul` (the ROM3 command ring), `chandler` (the command
dispatcher), `tprotocol`, `gconfig`, `reset` and `settings` are the template's, unchanged in what
they do; the template's documentation describes them. Two rules matter here: `chandler_loop()`
must run in every loop that can block, and nothing slow goes between a callback and the answer.

## Memory

| Region | Origin | Length | Here |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | The firmware: 491,612 bytes (a release build) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | The app settings, one 4 KB sector per app |
| `RAM` | `0x20000000` | 192 K | 71,152 bytes of static data; the heap is the rest, 125,456 bytes |
| `ROM_IN_RAM` | `0x20030000` | 64 K | The cartridge window |
| `SCRATCH_Y` | `0x20041000` | 4 K | Core 0's stack, with an MPU guard on its bottom 32 bytes |

The heap ends where the cartridge window starts, and malloc returns NULL instead of panicking:
every allocation checks for it. LIST's listing takes 26 KB of it from the first LIST on. Keep
large buffers off core 0's 4 KB stack: `macdisk.c` keeps its `FILINFO`, paths and write buffer
static.

## The cartridge window

The ST sees `ROM_IN_RAM` at `$FA0000`-`$FAFFFF`, read-only. The layout comes from the template
(`CHANDLER_*` in `chandler.h`, and their counterparts such as `RANDOM_TOKEN_ADDR` and
`APP_FREE_ADDR` in `target/atarist/src/inc/sidecart_layout.s`); never hard-code an address in it:

| ST address | Size | Contents |
| --- | --- | --- |
| `$FA0000` | 8 KB | The cartridge code (`main.s`) |
| `$FA2000` | 4 B | The sentinel: `CMD_RESET` for the boot screen once it has asked for Booster |
| `$FA2004` | 252 B | The random token and its seed (the commands' answers), a reserved slot, 60 shared variables |
| `$FA2100` | 512 B | The template's translation table: unused here |
| `$FA2300` | ~48 KB | `APP_FREE`: the Mac's status block and buffer |
| `$FAE0C0` | 8000 B | The template's framebuffer: unused here |

`APP_FREE` must stay at `$FA2300`: the Jackintosh ROMs read the status block there. In it
(`macdisk.h`, every field a long in the 68000's byte order):

| Offset | Contents |
| --- | --- |
| `+$00` | The last command's result: `MACDISK_OK` or a negative `MACDISK_ERR_*` |
| `+$04`, `+$08` | TIME: UTC seconds since 1904, and its flags (`MACDISK_TIME_FLAG_VALID`) |
| `+$0C` | `MACDISK_ID`, `$4A4B0001` ("JK", contract version 1), from the RP's boot on |
| `+$10` | The unit table: per unit, flags, sectors, change counter and format |
| `+$40` | The card's state (`MACDISK_CARD_*`) |
| `+$100` | The buffer: sector data, the PRAM, LIST's and GET_SETTINGS' answers |

The RP keeps the identity, the unit table and the card's state current: the Mac reads them at any
time without a command.

## Commands

A command id is the app number in the high byte and the command in the low byte. The Jackintosh
ROMs send app `$07` (the first app number no other SidecarTridge firmware uses):

| Id | Name | What it does |
| --- | --- | --- |
| `$0700` | READ | d3 = unit and count, d4 = first sector: up to 32 sectors into the buffer |
| `$0701` | WRITE | d3 = unit and count, d4 = first sector, then up to 2 sectors of data |
| `$0702` | EJECT | d3 = unit: closes the image until the ST boots again |
| `$0703` | TIME | Writes the time and its flags |
| `$0704` | PRAM_READ | Copies `PRAM.BIN` (256 bytes) into the buffer |
| `$0705` | PRAM_WRITE | 256 bytes of data: rewrites `PRAM.BIN` in place, synced before the answer |
| `$0706` | LIST | d3 = first entry, d4 = `MACDISK_LIST_WHOLE_CARD` or 0, then a folder's path |
| `$0707` | GET_SETTINGS | The 13 paths, FD0's current slot, the RTC settings, the "OPT1" marker and the ten ROM options into the buffer |
| `$0708` | SET_SETTING | d3 = key (`MACDISK_KEY_*`), then a path, an RTC value or a ROM option |

The cartridge's boot screen sends `$0002` (`EMUL_CMD_BOOSTER`, no payload). `$FF01`
(`CMD_ST_HELLO`) and `$FF00` (`CMD_SET_SHARED_VAR`) are the template's own, answered by chandler
before any callback.

What each side may assume:

- **Every command writes its result before it is answered**, and is harmless to repeat: a retry
  is a new command with a fresh token, and the RP may have run the first attempt already. Reads
  and writes name an absolute sector, an eject of an empty unit succeeds, the PRAM and a setting
  are written whole.
- **The answer is a two-phase commit**: the RP writes the token back, then a new seed, and the ST
  accepts the answer only when the token matches and the seed has moved.
- **The RP answers first**, from a main loop that drains the command ring on every pass; an LED
  write or a debug print goes after the answer. The slowest commands measured on the bench: a
  SET that writes the flash and swaps an image, about 120 ms; the first PRAM_WRITE, about 150 ms.
- **The two sides reboot independently.** The RP rewrites the window when it starts, so the
  identity and the unit table read as zero until it is back.

## The Atari ST side

`target/atarist/src/main.s` is the whole cartridge (all 8 KB, placed by `userfw.ld`). TOS runs it
after GEMDOS init (`CA_INIT` bit 27). It:

1. Turns a Mega STE's cache off (commands do not reach the RP with it on) and copies itself into
   the 4 KB below the screen, where it runs, so an RP restart or reflash under it does no harm.
2. Says hello (`CMD_ST_HELLO`) until the RP answers, then publishes the machine and TOS
   (`detect_hw`, `get_tos_version`).
3. Shows the boot screen with GEMDOS calls: a 40-column page (centred on 80-column screens) with
   the title bar, `JACKINTOSH` and the version (from `version.txt`, through `build/version.inc`,
   which the Makefile writes because vasm's `-D` takes no strings), the keys, the project's site
   and the copyright. A 30 s countdown runs on `_hz_200`; any key other than X or G stops it.
4. On X, or at the end of the countdown, sends `CMD_BOOSTER` until it is answered, waits up to
   3 s for `CMD_RESET` in the sentinel, and resets the ST; the RP jumps to Booster.
5. On G, puts the Mega STE's cache setting back and returns to TOS, which boots the GEM desktop.

## Testing

There is no off-hardware test suite: the RP side runs on the device. With the Debug Probe and
`tools/dev/console.py watch` capturing the console:

- `tools/dev/disktest_harness.py` runs the disk self-test on the RP itself (or put `SELFTEST.RUN`
  in the app folder and restart the RP; it writes `SELFTEST.LOG`).
- `tools/dev/macdisk_harness.py` injects the Mac's commands through the debug mailbox and checks
  what the 68000 would read, against host copies of the card's images; `--set` also checks the
  chooser's SET and the SELECT step.
- `tools/dev/tools_harness.py`, `select_harness.py` and `power_cycles.py` check the tools, the
  SELECT button and cold boots.

The harnesses' commands share the result slot and the buffer with the ST's own: run them with the
ST off or idle at the Jackintosh menu, never while the Mac uses its disks. The TOS boot screen
can be checked in Hatari, from a copy of `main.s` without the hello and `detect_hw`, which wait
for the RP forever. `tools/dev/README.md` has every command.

## Releasing

- `desc/app.json` is the descriptor template: `build.sh` fills in `<APP_UUID>`, `<APP_VERSION>`
  and `<BINARY_MD5_HASH>` on every build.
- The app needs its own UUID4 (today it shares the development one), and with it
  `ACONFIG_VERSION_NUMBER` back to 1.
- The version is in `version.txt`; `CHANGELOG.md` gets the release's entry.
- Tagging `v*` runs `.github/workflows/release.yml`, which builds, attaches the UF2 and the JSON
  to the GitHub release and uploads them; the tag is the maintainer's call.
