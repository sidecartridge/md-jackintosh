# Changelog

## v1.0.0 (unreleased) - md-jackintosh

The first version of md-jackintosh, built from the SidecarTridge
Multi-device microfirmware template v1.3.0 (its history is in that
repository). It serves the disks, the clock and the parameter RAM of the
Jackintosh ROMs, which turn an Atari ST or STE into a Macintosh Plus at
power-on with no TOS and talk to the cartridge themselves. Under TOS the
cartridge only shows a boot screen. Seen on an STE: System 6.0.8 boots from
a floppy image and from a hard disk image, Word 4.0a runs from the second
floppy and saves to it, a Restart comes back to the Finder, PageMaker 3.5
installs from five floppies stepped with SELECT, and Tetris runs.

### Added

- **The Mac's disks** (`rp/src/macdisk.c`, `rp/src/include/macdisk.h`):
  two floppies and a hard disk as 512-byte block devices, app `$07`:
  READ `$0700` (up to 32 sectors), WRITE `$0701` (up to 2 sectors, one
  `send_write_sync`) and EJECT `$0702`. At power-on the images the
  settings name are mounted (see the disk chooser below), and a unit with
  none stays empty: raw images read-write (a floppy only when its name ends
  in `.rw`, as in md-drives-emulator), DiskCopy 4.2 and MacBinary II
  DiskCopy 4.2 read-only. The status block at `$FA2300` holds the last
  result, an identity long
  (`$4A4B0001`) and, per unit, flags, sectors, a change counter, the
  format and the image's identity (the FNV-1a hash of its full path, 0
  when empty), so the Mac can tell the same image from another after the
  RP restarts; the card's state reads OK only once the units are
  published. Sector data is at `$FA2400`. Writes are synced 3 s after the
  first unsynced one. A unit an EJECT emptied, as a Mac's Restart and
  Shut Down do, is mounted again at the ST's next boot. `macdisk.h` is the
  contract with the ROMs.
- **The disk chooser** the ROMs draw, as md-drives-emulator's menu:
  LIST `$0706` lists a folder of the card, 16 entries at a time (".."
  first below the app folder, then folders, then files, sorted ignoring
  case, dot names hidden; or from "/", for choosing the app folder);
  GET_SETTINGS `$0707` and SET_SETTING `$0708` read and write the app
  folder, FD0's ten slots, FD1 and HD0 in the app settings, and ten
  option slots the ROMs keep for their own emulation options (printable
  ASCII, at most 15 bytes, after an "OPT1" marker): the menu's countdown,
  the Mac's RAM, the right mouse button, the screen, the keyboard layout,
  the mouse's tracking speed and a Mega STE's speed and cache. A SET is
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
  the app works only with the Jackintosh ROMs, so under TOS it shows a page
  that says so. X goes to Booster, G to the GEM desktop, after 30 s Booster
  starts on its own, and any other key stops the countdown. The version comes from
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

### Changed from the template

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

### Removed from the template

- The RP-drawn setup menu and everything behind it: `display.c`,
  `display_term.c`, `term.c`, `u8g2/`, and the user firmware module
  (`userfw.s`, `CMD_START`).
- The download stack and the HTTPS build profile (`download.c`, `httpc/`,
  `devdownload.c`, `md5/`, mbedTLS, `APP_DOWNLOAD_HTTPS`): the network
  is used for NTP only.
- The dev tools that drove the menu: `st_harness.py`, `sttest.s`,
  `download_harness.py`, and `swd.py`'s `screen`, `text` and `key`.
