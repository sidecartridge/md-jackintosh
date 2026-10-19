<div align="center">

<img src="icon.png" alt="SidecarTridge Multi-device Jackintosh Multidrive Emulation" width="180" />

# Jackintosh Multidrive Emulation

[![Build](https://github.com/sidecartridge/md-jackintosh/actions/workflows/build.yml/badge.svg)](https://github.com/sidecartridge/md-jackintosh/actions/workflows/build.yml)
[![Release](https://github.com/sidecartridge/md-jackintosh/actions/workflows/release.yml/badge.svg)](https://github.com/sidecartridge/md-jackintosh/actions/workflows/release.yml)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)

</div>

A [SidecarTridge Multi-device](https://sidecartridge.com) microfirmware that serves the
**floppy and hard disk images**, the **clock** and the **parameter RAM** of the **Jackintosh
ROMs**, which turn an Atari ST, STe, Mega ST or Mega STe into a **Macintosh Plus** at power-on,
with no TOS at all.

> Learn how to install and use it: <https://jackintosh.sidecartridge.com>

> 🛒 **Get the hardware:** [SidecarTridge Multi-device](https://sidecartridge.com/products/sidecartridge-multidevice-atari-st/)
> and [SidecarTridge TOS Emulator](https://sidecartridge.com/products/sidecartridge-tos-atari-st/)

## How it works

Two parts work together:

- **The Jackintosh ROMs** ([md-jackintosh-roms](https://github.com/sidecartridge/md-jackintosh-roms)),
  built at <https://jackintosh.sidecartridge.com>. They take the place of TOS in the Atari's ROM
  sockets, boot it as a Mac Plus, draw the disk menu, and drive the cartridge port themselves.
- **This microfirmware**, on the Multi-device in the cartridge port. It reads and writes the
  Mac's disk images on the microSD card, gives the Mac the time of day from an NTP server, keeps
  its parameter RAM, and stores the choices made in the menu.

### Why a TOS Emulator

An Atari runs whatever its ROM sockets hold at power-on, so the Jackintosh ROMs have to sit where
TOS sits. The [SidecarTridge TOS Emulator](https://sidecartridge.com/products/sidecartridge-tos-atari-st/)
replaces the TOS chips with a board that serves ROM images from its own storage: load the
Jackintosh ROMs into it, and switch between them and TOS without opening the computer again.

You can also burn the ROMs built at <https://jackintosh.sidecartridge.com> into EPROMs or
EEPROMs and fit them in the TOS sockets instead of the TOS chips.

## Installation

To install the Jackintosh Multidrive Emulation app on your SidecarTridge Multi-device:

1. Launch the **Booster App** on your SidecarTridge Multi-device.
2. Open the Booster web interface.
3. In the **Apps** tab, select **"Jackintosh Multidrive Emulation"** from the list of available
   apps.
4. Click **"Download"** to install the app to your SidecarTridge's microSD card.
5. Once installed, select the app and click **"Launch"** to activate it.

After launching, the app will automatically run every time your Atari computer is powered on.
It only works with the Jackintosh ROMs: an Atari that boots TOS shows a page inviting you back to
Booster or on to the GEM desktop (see [Without the Jackintosh ROMs](#without-the-jackintosh-roms)).

Then put the Jackintosh ROMs in the TOS sockets: build them at
<https://jackintosh.sidecartridge.com>, and load them into the
[SidecarTridge TOS Emulator](https://sidecartridge.com/products/sidecartridge-tos-atari-st/), or
burn them into EPROMs or EEPROMs.

## Usage

Every time your Atari boots with the Jackintosh ROMs, they show the **Jackintosh menu** for 20
seconds. If no key is pressed, the Mac boots from the disks chosen in it. Any key stops the
countdown; **`E`** then boots.

On first use, the microfirmware creates the folder **`/jackintosh`** on the microSD card if it
does not exist. Put your Mac disk images there, in any subfolders you like, and choose them in
the menu. Nothing is mounted until it is chosen: a drive with no image chosen stays empty.

### Disk images

| Unit | Mac drive | Images |
|------|-----------|--------|
| **FD0** | Internal floppy | Ten slots: slot 1 at power-on, the others stepped with **`SELECT`** |
| **FD1** | External floppy | One image |
| **HD0** | Hard disk | One image |

- **Raw images** (`.dsk`, `.img`, …): the file is the disk, sector by sector.
- **DiskCopy 4.2** images and **MacBinary II** files holding a DiskCopy 4.2 image (`.image.bin`):
  always read-only.
- **A floppy is read-only unless its name ends in `.rw`** (in any case), as in the Drives
  Emulator: `Word.dsk` is read-only, `Word.dsk.rw` is written. The hard disk is read-write, unless
  the file is read-only on the card.
- The format is told from the content, not from the extension.

The Mac's parameter RAM (the control panel settings, the time zone) is kept in
`/jackintosh/PRAM.BIN`.

## The Jackintosh menu

The menu is drawn by the Jackintosh ROMs; this microfirmware lists the card for it and keeps
what you choose in the Multi-device's flash, so the choices survive power cycles.

### Disk commands

| Command | Description |
|---------|-------------|
| **Fo[l]der** | Choose the folder the images are browsed from. By default `/jackintosh`, created on first use. The browser starts from the root of the card. |
| **[A] FD0** | Choose the image of FD0's current slot. |
| **[CTRL+A] Configure multiple images** | Configure up to 10 images for FD0. Slot 1 is the main **[A] FD0** image; press `2..9` or `0` to choose slots 2..10, and `SHIFT + 2..9` or `0` to empty a slot. |
| **[B] FD1** | Choose the image of FD1. |
| **[I] HD0** | Choose the hard disk image. |
| **SHIFT+A, SHIFT+B, SHIFT+I** | Empty FD0's current slot, FD1 or HD0. |
| **[E]xit to the Mac** | Boot the Mac with the chosen disks. |

The menu shows each drive's state: read-only, not mounted (the image cannot be opened), or a
disk mounted that is not the one chosen.

### Real-time clock commands

| Command | Description |
|---------|-------------|
| **[R]TC Enabled** | Enable or disable the time of day from NTP. |
| **[H]ost NTP** | The NTP server to ask (default `pool.ntp.org`). |
| **[P]ort** | The NTP server's port (default 123). |
| **[U] Offset** | The Mac's time zone, kept in its parameter RAM. |

### Browsing the microSD card

| Key | Action |
|-----|--------|
| **`UP`** | Move up in the list |
| **`DOWN`** | Move down in the list |
| **`LEFT`** | Previous page |
| **`RIGHT`** | Next page |
| **`RETURN`** | Open a folder, or `..` to go back to the parent folder |
| **`SPACE`** | Choose the selected image, or the folder |
| **`ESC`** | Leave the browser |

Names starting with `.` are not listed. A folder lists at most 256 entries, and paths longer than
95 characters cannot be chosen.

## SELECT button

Once the Jackintosh ROMs have started, in the menu or in the Mac, a short press on **`SELECT`**
changes the disk in FD0:

- **With FD0 empty** (eject the floppy on the Mac first), the next slot that holds an image goes
  in, and the Pico W LED flashes the slot's number. Empty slots are skipped, and after the last
  slot it wraps back to the first.
- **With a disk in FD0**, nothing happens: swapping a disk under a mounted Mac volume would
  corrupt it.

Under TOS, or while the LED blinks for a missing card, a short press restarts the Multi-device.
Holding **`SELECT`** for 10 seconds is a factory reset: the global settings are erased and the
Multi-device restarts into Booster.

## Real-time clock

The Mac gets the time of day from an NTP server, over the Wi-Fi network configured in the
**Booster App** (STA mode). The microfirmware keeps serving UTC; the Mac applies its own time
zone. Until a server has answered, the Mac keeps its own clock.

The RTC settings apply at the Multi-device's next start: **after a power cycle of the computer or
a reset of the SidecarTridge Multi-device**. The Atari's reset button is not enough. With the RTC
disabled, the microfirmware does not start the Wi-Fi at all.

## The LED

| LED | Meaning |
|-----|---------|
| Off, with short flashes | Working: it flashes on every access to the microSD card for the Mac |
| N short flashes | `SELECT` put FD0's slot N in the drive |
| Blinking every 700 ms | No usable microSD card: none inserted, no FAT or exFAT file system, or the folder cannot be created. Nothing else starts; the Jackintosh ROMs show the reason. Insert a card and press **`SELECT`** |

## Without the Jackintosh ROMs

This microfirmware works only together with the Jackintosh ROMs running in the TOS sockets: they
are the Mac, and it is their disks, clock and parameter RAM. Launched on an Atari that boots TOS
instead, it has nothing to serve, so the cartridge shows a page that says so and invites you to
go back to Booster or on to the GEM desktop:

| Key | Action |
|-----|--------|
| **`X`** | Return to the Booster menu |
| **`G`** | Continue to the GEM desktop |
| any other key | Stop the countdown; then only `X` or `G` leave |

After 30 seconds without a key, it returns to Booster. Every boot under TOS shows it again. The
Jackintosh ROMs never run the cartridge's code, so with them the page never appears.

## System reset behavior

Pressing the reset button on your Atari shows the Jackintosh menu again and boots the Mac with the
same disks. A floppy the Mac ejected (Restart and Shut Down eject them) is put back in its drive,
so the next boot finds it. FD0 keeps the slot `SELECT` last put in.

## Power cycling

After a power cycle the Jackintosh menu shows the last choices, with FD0 back at slot 1. The RTC
settings changed since the last start apply now.

## Advanced features

### Changing the speed of the microSD card

The microSD card speed is the SPI bus clock between the RP2040 and the card, from **1 MHz** to
**24 MHz**; the default is **12.5 MHz**. It is a global SidecarTridge Multi-device setting, set
in the **Booster App**: in its web interface, the **Config** tab, **SD card bus speed (KHz)**
(for example `24000` for 24 MHz). Save it and relaunch the app. If you see instability, try
`12500` or `6000`.

## Building

Build with the repo-root `build.sh` (see `CLAUDE.md` for the full toolchain requirements):

```bash
# ./build.sh <board_type> <build_type> <app_uuid>
./build.sh pico_w release <app-uuid>
```

The build produces `dist/<uuid>-<version>.uf2` (the firmware you flash) and `dist/<uuid>.json`
(the app descriptor from `desc/app.json`).

This project is based on the [SidecarTridge Multi-device Microfirmware App Template](https://github.com/sidecartridge/md-microfirmware-template).
To set up your development environment, please follow the instructions provided in the
[official documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

## Project docs

- `programming.md`: how the microfirmware is built, and the contract with the Jackintosh ROMs.
- `rp/src/include/macdisk.h`: the commands, the window layout and the results the Jackintosh ROMs
  use.
- `CLAUDE.md` / `AGENTS.md`: build, architecture, and contributor playbook.
- `tools/dev/README.md`: the debug tools and the device harnesses.

## License

Licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

## Contributing

Made with ❤️ by [SidecarTridge](https://sidecartridge.com)
