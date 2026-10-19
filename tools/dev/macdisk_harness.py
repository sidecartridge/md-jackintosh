#!/usr/bin/env python3
"""Check the Mac disk commands (rp/src/macdisk.c) on the device.

The firmware mounts its settings' images at power-on; a unit with none stays
empty. Give the host's copies of the mounted files with --fd0, --fd1 and
--hd0: every case compares what the 68000 would read in the cartridge window
with them.

  ID      the identity long is MACDISK_ID
  STATUS  each unit given is inserted, with the format, sector count and
          read-only flag its host copy implies
  READ    the first and the last MACDISK_MAX_READ_SECTORS of each unit and
          --samples runs in between (--full: every sector), byte for byte
  ERRORS  past the end, across the end, counts of 0 and one over the limit, a
          unit and a command that do not exist
  RO      a write to a read-only unit is refused and changes nothing
  WRITE   (--write) two sectors of HD0 rewritten, read back, written again (a
          retry), read back, and put back as they were
  EJECT   (--eject UNIT) the unit empties, its change counter moves once, a
          second eject succeeds and a read is refused. The unit stays empty
          until the ST's next boot (CMD_ST_HELLO) mounts it again.
  PRAM    (--pram) PRAM.BIN written, read back, written again (a retry), a
          wrong size refused, and put back as it was. If there was none, it is
          left all zeros, which the Mac takes for PRAM never set.
  TIME    MACDISK_CMD_TIME answers; once NTP has answered, the time is within
          a minute of this computer's clock, else it is 0 and not valid
  GET     MACDISK_CMD_GET_SETTINGS: FOLDER is a path, the slot is 1-10, the
          "OPT1" marker is there before the ROM options
  LIST    every page of FOLDER and of its first subfolder: the totals add up,
          ".." first only below FOLDER, folders then files sorted ignoring
          case, no name starting with ".", the .rw and too-long flags match
          the names; a page past the end is refused
  REFUSE  LIST and SET_SETTING refuse a path above FOLDER, one through "..",
          one through a backslash, one that is not there, a folder as an
          image, a file as FOLDER, an empty FOLDER, an unknown key, bad
          RTC values and a ROM option too long; the settings are unchanged
          after
  SET     (--set) writes the settings flash, and puts it back: FD1 set to an
          image of FOLDER is swapped at once (its change counter moves, a
          floppy not named *.rw is read-only) and put back; the slot after
          FD0's current one set to an image leaves FD0 alone, and after a
          hello (injected) and an eject a short SELECT press puts it in FD0.
          Then the settings are put back and the RP restarted, so the units
          and FD0's current slot are as at power-on, and GET is checked again
  BUSY    the slowest command the RP answered since it booted
          (chandlerMaxBusyUs): the Mac ROM's timeout must cover it

The commands go through the devhooks mailbox, as if the ST had sent them, so
it needs a debug build and the Debug Probe, and no ST. They share the result
slot and the buffer with the ST's own commands: run it with the ST off or idle
at the Jackintosh ROM's menu, never while the Mac uses its disks.

    python3 tools/dev/macdisk_harness.py --fd0 System.image --hd0 HD0.hda
    python3 tools/dev/macdisk_harness.py --hd0 HD0.hda --write --full --pram
    python3 tools/dev/macdisk_harness.py --set

Exits 0 when every check passes. Writes tools/dev/logs/macdisk-harness-<time>.json.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import random
import struct
import sys
import time

import swd

UNITS = ("FD0", "FD1", "HD0")
MARKER = 0x7FFF7FFF  # in the result slot before a command; no result has it
SECTOR = 512
# Seconds from the Mac's epoch to the Unix one, worked out here to check the RP.
MAC_TO_UNIX = int((datetime.datetime(1970, 1, 1) - datetime.datetime(1904, 1, 1)).total_seconds())


def st_view(raw: bytes) -> bytes:
    """The window's bytes in the order the 68000 reads them: the RP serves
    16-bit words, the high byte at the even address."""
    out = bytearray(len(raw))
    out[0::2], out[1::2] = raw[1::2], raw[0::2]
    return bytes(out)


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def host_layout(path: str) -> dict:
    """Format, data offset and sectors of a host image, by the rules in
    macdisk.h, written again here to check the firmware against."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(128 + 0x54)

    def dc42(h: bytes, limit: int) -> int | None:
        if limit < 0x54 or len(h) < 0x54 or h[0] > 63 or h[0x52:0x54] != b"\x01\x00":
            return None
        data, tags = be32(h, 0x40), be32(h, 0x44)
        if data == 0 or data % SECTOR or 0x54 + data + tags > limit:
            return None
        return data

    if (len(head) == 128 + 0x54 and head[0] == 0 and head[74] == 0 and head[82] == 0
            and 1 <= head[1] <= 63 and 128 + be32(head, 83) <= size):
        data = dc42(head[128:], be32(head, 83))
        if data is not None:
            return {"format": "MACDISK_FORMAT_MACBINARY_DC42", "offset": 128 + 0x54,
                    "sectors": data // SECTOR, "readonly": True}
    data = dc42(head, size)
    if data is not None:
        return {"format": "MACDISK_FORMAT_DC42", "offset": 0x54,
                "sectors": data // SECTOR, "readonly": True}
    return {"format": "MACDISK_FORMAT_RAW", "offset": 0, "sectors": size // SECTOR,
            "readonly": None}  # the card's file attribute decides


class Device:
    def __init__(self, elf: str):
        self.elf = elf
        self.defs = swd.include_defines()
        self.window = swd.cartridge_window(elf)

    def d(self, name: str) -> int:
        return self.defs[name]

    def st_long(self, offset: int) -> int:
        return struct.unpack(">i", st_view(swd.read_memory(self.window + offset, 4)))[0]

    def st_ulong(self, offset: int) -> int:
        return struct.unpack(">I", st_view(swd.read_memory(self.window + offset, 4)))[0]

    def unit(self, index: int) -> dict:
        base = self.d("MACDISK_UNIT_TABLE_OFFSET") + index * self.d("MACDISK_UNIT_ENTRY_SIZE")
        raw = st_view(swd.read_memory(self.window + base, 16))
        flags, sectors, changes, fmt = struct.unpack(">IIII", raw)
        return {"flags": flags, "sectors": sectors, "changes": changes, "format": fmt}

    def command(self, cmd: str | int, unit: int, count: int = 0, lba: int = 0,
                data: bytes = b"") -> int:
        """Send one command, a macdisk.h name or an id, as send_sync and
        send_write_sync build it (d3, d4, d5, then the data), and return its
        result once it is written."""
        result_addr = self.window + self.d("MACDISK_RESULT_OFFSET")
        # The marker in the ST's byte order: no result has it.
        swd.openocd(f"mww 0x{result_addr:08x} 0x{MARKER:08x}")
        d3 = unit | (count << 16)
        words = [d3 & 0xFFFF, d3 >> 16, lba & 0xFFFF, lba >> 16, 0, 0]
        words += [(data[i] << 8) | data[i + 1] for i in range(0, len(data), 2)]
        swd.send_protocol(self.elf, self.d(cmd) if isinstance(cmd, str) else cmd, words)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = self.st_long(self.d("MACDISK_RESULT_OFFSET"))
            if result != MARKER:
                return result
            time.sleep(0.1)
        raise swd.SwdError(f"no result for {cmd} within 10 s")

    def read(self, unit: int, lba: int, count: int) -> tuple[int, bytes]:
        result = self.command("MACDISK_CMD_READ", unit, count, lba)
        if result != self.d("MACDISK_OK"):
            return result, b""
        raw = swd.read_memory(self.window + self.d("MACDISK_BUFFER_OFFSET"), count * SECTOR)
        return result, st_view(raw)

    def pram(self) -> tuple[int, bytes]:
        size = self.d("MACDISK_PRAM_SIZE")
        result = self.command("MACDISK_CMD_PRAM_READ", 0)
        if result != self.d("MACDISK_OK"):
            return result, b""
        return result, st_view(swd.read_memory(self.window + self.d("MACDISK_BUFFER_OFFSET"), size))

    def path_data(self, path: str) -> bytes:
        data = path.encode("cp437") + b"\0"
        return data + b"\0" * (len(data) & 1)

    def text(self, offset: int, size: int) -> str:
        raw = st_view(swd.read_memory(self.window + offset, size))
        return raw.split(b"\0", 1)[0].decode("cp437")

    def get_settings(self) -> tuple[int, list[str], int]:
        """GET_SETTINGS: its result, the MACDISK_KEYS values in key order (the
        paths, the RTC keys', the ROM options; None for the options when the
        "OPT1" marker is missing) and FD0's slot."""
        result = self.command("MACDISK_CMD_GET_SETTINGS", 0)
        if result != self.d("MACDISK_OK"):
            return result, [], 0
        size, paths = self.d("MACDISK_PATH_SIZE"), self.d("MACDISK_PATH_KEYS")
        base = self.d("MACDISK_BUFFER_OFFSET")
        raw = st_view(swd.read_memory(self.window + base, self.d("MACDISK_SETTINGS_SIZE")))
        rtc = self.d("MACDISK_SETTINGS_RTC_OFFSET")
        fields = [(k * size, size) for k in range(paths)]
        fields += [(rtc + k * size, size) for k in range(self.d("MACDISK_RTC_KEYS"))]
        opt_size = self.d("MACDISK_OPTION_SIZE")
        opt = self.d("MACDISK_SETTINGS_OPTION_FIELDS")
        fields += [(opt + n * opt_size, opt_size) for n in range(self.d("MACDISK_OPTIONS"))]
        values = [raw[o:o + n].split(b"\0", 1)[0].decode("cp437") for o, n in fields]
        if be32(raw, self.d("MACDISK_SETTINGS_OPTIONS_OFFSET")) != self.d("MACDISK_OPTIONS_MAGIC"):
            values[paths + self.d("MACDISK_RTC_KEYS"):] = [None] * self.d("MACDISK_OPTIONS")
        return result, values, be32(raw, self.d("MACDISK_SETTINGS_FD0_SLOT_OFFSET"))

    def list_page(self, path: str, first: int) -> tuple[int, dict]:
        result = self.command("MACDISK_CMD_LIST", first & 0xFFFF, first >> 16, 0,
                              self.path_data(path))
        if result != self.d("MACDISK_OK"):
            return result, {}
        base = self.d("MACDISK_BUFFER_OFFSET")
        head = st_view(swd.read_memory(self.window + base, 12))
        total, returned, flags = struct.unpack(">III", head)
        size = self.d("MACDISK_LIST_ENTRY_SIZE")
        raw = st_view(swd.read_memory(
            self.window + base + self.d("MACDISK_LIST_ENTRIES"), returned * size)) \
            if returned else b""
        entries = []
        for i in range(returned):
            e = raw[i * size:(i + 1) * size]
            name = e[self.d("MACDISK_ENTRY_NAME"):self.d("MACDISK_PATH_SIZE")]
            entries.append({"name": name.split(b"\0", 1)[0].decode("cp437"),
                            "flags": be32(e, self.d("MACDISK_ENTRY_FLAGS")),
                            "bytes": be32(e, self.d("MACDISK_ENTRY_BYTES"))})
        return result, {"total": total, "returned": returned, "flags": flags,
                        "entries": entries}

    def list_all(self, path: str) -> tuple[int, dict]:
        """Every page of a folder, from the first."""
        result, page = self.list_page(path, 0)
        if result != self.d("MACDISK_OK"):
            return result, {}
        entries = list(page["entries"])
        while page["returned"] and len(entries) < page["total"]:
            result, page = self.list_page(path, len(entries))
            if result != self.d("MACDISK_OK"):
                return result, {}
            entries += page["entries"]
        return result, {"total": page["total"], "flags": page["flags"],
                        "entries": entries}

    def set_setting(self, key: int, path: str) -> int:
        return self.command("MACDISK_CMD_SET_SETTING", key, 0, 0, self.path_data(path))

    def max_busy_us(self) -> int:
        addr = swd.elf_symbols(self.elf, "chandlerMaxBusyUs")["chandlerMaxBusyUs"][0]
        return struct.unpack("<I", swd.read_memory(addr, 4))[0]


def child(folder: str, name: str) -> str:
    return ("" if folder == "/" else folder) + "/" + name


def check_listing(dev: Device, folder: str, top: str, listing: dict) -> list[str]:
    """What is wrong with a whole listing of folder, by macdisk.h's rules."""
    bad = []
    entries = listing["entries"]
    is_folder = dev.d("MACDISK_ENTRY_FOLDER")
    if len(entries) != listing["total"]:
        bad.append(f"{len(entries)} entries in the pages, total {listing['total']}")
    below = folder.lower() != top.lower()
    has_parent = bool(entries) and entries[0]["name"] == ".."
    if has_parent != below:
        bad.append("'..' " + ("missing" if below else "at the top"))
    rest = entries[1:] if has_parent else entries
    key = [(0 if e["flags"] & is_folder else 1, e["name"].encode("cp437").lower())
           for e in rest]
    if key != sorted(key):
        bad.append("not sorted: folders first, then names ignoring case")
    for e in rest:
        name = e["name"]
        folderish = bool(e["flags"] & is_folder)
        rw = not folderish and name.lower().endswith(".rw") and len(name) > 3
        too_long = len(child(folder, name).encode("cp437")) >= dev.d("MACDISK_PATH_SIZE")
        want = ((is_folder if folderish else 0) | (dev.d("MACDISK_ENTRY_RW") if rw else 0)
                | (dev.d("MACDISK_ENTRY_TOO_LONG") if too_long else 0))
        if name.startswith("."):
            bad.append(f"{name}: a dot name listed")
        elif e["flags"] != want and not too_long:
            bad.append(f"{name}: flags {e['flags']}, want {want}")
    return bad


def result_name(dev: Device, value: int) -> str:
    names = [n for n, v in dev.defs.items()
             if v == value and (n == "MACDISK_OK" or n.startswith("MACDISK_ERR_"))]
    return names[0] if names else str(value)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    for name in UNITS:
        ap.add_argument(f"--{name.lower()}", metavar="IMAGE",
                        help=f"the host's copy of the card's {name}.* image")
    ap.add_argument("--samples", type=int, default=4, help="random reads per unit")
    ap.add_argument("--full", action="store_true", help="read every sector")
    ap.add_argument("--write", action="store_true",
                    help="rewrite two sectors of HD0 and put them back")
    ap.add_argument("--eject", choices=UNITS, help="eject this unit at the end")
    ap.add_argument("--pram", action="store_true",
                    help="rewrite PRAM.BIN and put it back")
    ap.add_argument("--set", action="store_true",
                    help="change the image settings (the settings flash), put "
                         "them back, and restart the RP")
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--elf")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    results: list[dict] = []

    def check(case: str, ok: bool, detail: str, **extra) -> None:
        print(f"  {'PASS' if ok else 'FAIL'}  {case}: {detail}")
        results.append({"case": case, "ok": ok, "detail": detail, **extra})

    try:
        elf = swd.matching_elf(args.elf)
        dev = Device(elf)
        max_read = dev.d("MACDISK_MAX_READ_SECTORS")
        ok_code = dev.d("MACDISK_OK")
        print(f"build {os.path.basename(elf)}, seed {args.seed}")
        hosts = {i: getattr(args, n.lower()) for i, n in enumerate(UNITS)}
        layouts = {i: host_layout(p) for i, p in hosts.items() if p}
        status = {i: dev.unit(i) for i in range(len(UNITS))}

        ident = dev.st_ulong(dev.d("MACDISK_ID_OFFSET"))
        check("ID", ident == dev.d("MACDISK_ID"),
              f"0x{ident:08x}, want 0x{dev.d('MACDISK_ID'):08x}")

        for i, name in enumerate(UNITS):
            s = status[i]
            if i not in layouts:
                print(f"  ----  STATUS {name}: {s} (no host copy given)")
                continue
            want = layouts[i]
            inserted = bool(s["flags"] & dev.d("MACDISK_FLAG_INSERTED"))
            readonly = bool(s["flags"] & dev.d("MACDISK_FLAG_READONLY"))
            ok = (inserted and s["sectors"] == want["sectors"]
                  and s["format"] == dev.d(want["format"])
                  and (want["readonly"] is None or readonly == want["readonly"]))
            check(f"STATUS {name}", ok, f"{s}, host copy {want}")

        for i, name in enumerate(UNITS):
            if i not in layouts or not status[i]["flags"] & dev.d("MACDISK_FLAG_INSERTED"):
                continue
            sectors, offset = layouts[i]["sectors"], layouts[i]["offset"]
            if args.full:
                runs = list(range(0, sectors, max_read))
            else:
                runs = [0, max(0, sectors - max_read)]
                runs += [rng.randrange(0, max(1, sectors - max_read)) for _ in range(args.samples)]
            bad = []
            with open(hosts[i], "rb") as f:
                for lba in runs:
                    count = min(max_read, sectors - lba)
                    result, got = dev.read(i, lba, count)
                    f.seek(offset + lba * SECTOR)
                    want = f.read(count * SECTOR)
                    if result != ok_code:
                        bad.append(f"{lba}+{count}: {result_name(dev, result)}")
                    elif got != want:
                        bad.append(f"{lba}+{count}: data differs")
            check(f"READ {name}", not bad, f"{len(runs)} reads" +
                  (f", wrong at {', '.join(bad)}" if bad else ", all match the host copy"))

        target = next((i for i in (2, 0, 1) if status[i]["sectors"]), None)
        if target is not None:
            n = status[target]["sectors"]
            cases = [("past the end", "MACDISK_CMD_READ", target, 1, n, "MACDISK_ERR_RANGE"),
                     ("across the end", "MACDISK_CMD_READ", target, 2, n - 1, "MACDISK_ERR_RANGE"),
                     ("count 0", "MACDISK_CMD_READ", target, 0, 0, "MACDISK_ERR_PARAM"),
                     ("count over", "MACDISK_CMD_READ", target, max_read + 1, 0, "MACDISK_ERR_PARAM"),
                     ("no such unit", "MACDISK_CMD_READ", len(UNITS), 1, 0, "MACDISK_ERR_PARAM")]
            bad = []
            for label, cmd, unit, count, lba, want in cases:
                got = dev.command(cmd, unit, count, lba)
                if got != dev.d(want):
                    bad.append(f"{label}: {result_name(dev, got)}, not {want}")
            unknown = dev.command((dev.d("APP_MACDISK") << 8) | 0x7F, target, 1, 0)
            if unknown != dev.d("MACDISK_ERR_PARAM"):
                bad.append(f"unknown command: {result_name(dev, unknown)}")
            check("ERRORS", not bad, "; ".join(bad) or f"{len(cases) + 1} refusals as expected")

        ro = next((i for i in range(len(UNITS))
                   if status[i]["flags"] & dev.d("MACDISK_FLAG_READONLY")), None)
        if ro is not None:
            _, before = dev.read(ro, 0, 1)
            got = dev.command("MACDISK_CMD_WRITE", ro, 1, 0, bytes(SECTOR))
            _, after = dev.read(ro, 0, 1)
            check(f"RO {UNITS[ro]}", got == dev.d("MACDISK_ERR_READONLY") and before == after,
                  f"write answered {result_name(dev, got)}, sector 0 "
                  f"{'unchanged' if before == after else 'CHANGED'}")

        if args.write:
            hd = 2
            s = status[hd]
            if not s["sectors"] or s["flags"] & dev.d("MACDISK_FLAG_READONLY"):
                check("WRITE", False, f"needs a writable HD0, got {s}")
            else:
                lba = rng.randrange(0, s["sectors"] - 1)
                _, original = dev.read(hd, lba, 2)
                pattern = bytes(rng.randrange(256) for _ in range(2 * SECTOR))
                steps = []
                for label in ("write", "retry"):
                    got = dev.command("MACDISK_CMD_WRITE", hd, 2, lba, pattern)
                    _, back = dev.read(hd, lba, 2)
                    steps.append((label, got == ok_code and back == pattern,
                                  result_name(dev, got)))
                got = dev.command("MACDISK_CMD_WRITE", hd, 2, lba, original)
                _, back = dev.read(hd, lba, 2)
                restored = got == ok_code and back == original
                check("WRITE HD0", steps[0][1] and restored,
                      f"sectors {lba}+2: {steps[0][2]}, read back "
                      f"{'matches' if steps[0][1] else 'DIFFERS'}; put back "
                      f"{'as they were' if restored else 'WRONG'}")
                check("REPEAT HD0", steps[1][1], f"the same write again: {steps[1][2]}")

        if args.eject:
            unit = UNITS.index(args.eject)
            before = dev.unit(unit)
            first = dev.command("MACDISK_CMD_EJECT", unit)
            mid = dev.unit(unit)
            second = dev.command("MACDISK_CMD_EJECT", unit)
            after = dev.unit(unit)
            read = dev.command("MACDISK_CMD_READ", unit, 1, 0)
            want_changes = before["changes"] + (1 if before["flags"] else 0)
            ok = (first == ok_code and second == ok_code and mid["flags"] == 0
                  and mid["sectors"] == 0 and mid["changes"] == want_changes
                  and after["changes"] == want_changes
                  and read == dev.d("MACDISK_ERR_NOMEDIUM"))
            check(f"EJECT {args.eject}", ok, f"before {before}, after {after}, "
                  f"then a read answered {result_name(dev, read)}")

        if args.pram:
            size = dev.d("MACDISK_PRAM_SIZE")
            first, original = dev.pram()
            had = first == ok_code
            pattern = bytes(rng.randrange(256) for _ in range(size))
            steps = []
            for label in ("write", "retry"):
                got = dev.command("MACDISK_CMD_PRAM_WRITE", 0, data=pattern)
                back_result, back = dev.pram()
                steps.append(got == ok_code and back_result == ok_code and back == pattern)
            short = dev.command("MACDISK_CMD_PRAM_WRITE", 0, data=pattern[:size - 2])
            restore = original if had else bytes(size)
            got = dev.command("MACDISK_CMD_PRAM_WRITE", 0, data=restore)
            back_result, back = dev.pram()
            restored = got == ok_code and back == restore
            check("PRAM", (had or first == dev.d("MACDISK_ERR_NOMEDIUM")) and steps[0]
                  and short == dev.d("MACDISK_ERR_PARAM") and restored,
                  f"before: {result_name(dev, first)}; written and read back "
                  f"{'matches' if steps[0] else 'DIFFERS'}; a short write answered "
                  f"{result_name(dev, short)}; put back "
                  f"{('as it was' if had else 'as zeros') if restored else 'WRONG'}")
            check("REPEAT PRAM", steps[1], "the same write again "
                  f"{'reads back the same' if steps[1] else 'FAILED'}")

        got = dev.command("MACDISK_CMD_TIME", 0)
        seconds = dev.st_ulong(dev.d("MACDISK_TIME_SECONDS_OFFSET"))
        flags = dev.st_ulong(dev.d("MACDISK_TIME_FLAGS_OFFSET"))
        if flags & dev.d("MACDISK_TIME_FLAG_VALID"):
            skew = seconds - MAC_TO_UNIX - time.time()
            when = datetime.datetime.fromtimestamp(seconds - MAC_TO_UNIX, datetime.timezone.utc)
            check("TIME", got == ok_code and abs(skew) < 60,
                  f"{when:%Y-%m-%d %H:%M:%S} UTC, {skew:+.0f} s from this computer",
                  skew_s=round(skew))
        else:
            check("TIME", got == ok_code and seconds == 0 and flags == 0,
                  f"{result_name(dev, got)}, not synced yet (no Wi-Fi, or no NTP "
                  f"answer yet): seconds {seconds}, flags {flags}")

        param = dev.d("MACDISK_ERR_PARAM")
        got, settings, slot = dev.get_settings()
        folder = settings[dev.d("MACDISK_KEY_FOLDER")] if settings else ""
        check("GET", got == ok_code and folder.startswith("/") and 1 <= slot <= 10
              and None not in settings,
              f"{result_name(dev, got)}: FOLDER {folder!r}, FD0 slot {slot}, "
              f"FD0 slots {settings[1:11]}, FD1 {settings[11:12]}, HD0 {settings[12:13]}, "
              f"RTC {settings[13:16]}, options {settings[16:26]}")
        if got == ok_code:
            top = folder.rstrip("/") or "/"
            got, listing = dev.list_all(top)
            bad = check_listing(dev, top, top, listing) if got == ok_code else []
            files = [e for e in listing.get("entries", [])
                     if not e["flags"] & (dev.d("MACDISK_ENTRY_FOLDER") | dev.d("MACDISK_ENTRY_TOO_LONG"))]
            check(f"LIST {top}", got == ok_code and not bad,
                  f"{result_name(dev, got)}, {listing.get('total')} entries"
                  + (f", flags {listing.get('flags')}" if listing.get("flags") else "")
                  + (": " + "; ".join(bad) if bad else ", as macdisk.h says"))
            sub = next((e["name"] for e in listing.get("entries", [])
                        if e["flags"] == dev.d("MACDISK_ENTRY_FOLDER") and e["name"] != ".."), None)
            if sub:
                path = child(top, sub)
                got, inner = dev.list_all(path)
                bad = check_listing(dev, path, top, inner) if got == ok_code else []
                check(f"LIST {path}", got == ok_code and not bad,
                      f"{result_name(dev, got)}, {inner.get('total')} entries"
                      + (": " + "; ".join(bad) if bad else ", '..' first"))
            past = dev.list_page(top, listing.get("total", 0) + 1)[0]

            refusals = [("LIST past the end", past)]
            if top != "/":
                refusals.append(("LIST /", dev.list_page("/", 0)[0]))
            refusals += [
                ("LIST through '..'", dev.list_page(top + "/..", 0)[0]),
                ("LIST through a backslash", dev.list_page(top + "\\..", 0)[0]),
                ("LIST a missing folder", dev.list_page(top + "/no such folder", 0)[0]),
                ("SET an unknown key", dev.set_setting(dev.d("MACDISK_KEYS"), child(top, "x"))),
                ("SET FD1 to a missing file", dev.set_setting(dev.d("MACDISK_KEY_FD1"),
                                                              child(top, "no such image"))),
                ("SET FD1 to a folder", dev.set_setting(dev.d("MACDISK_KEY_FD1"), top)),
                ("SET FD1 through '..'", dev.set_setting(dev.d("MACDISK_KEY_FD1"),
                                                         top + "/../x.dsk")),
                ("SET an empty FOLDER", dev.set_setting(dev.d("MACDISK_KEY_FOLDER"), "")),
                ("SET RTC_ENABLED to 'yes'", dev.set_setting(dev.d("MACDISK_KEY_RTC_ENABLED"), "yes")),
                ("SET a host with a space", dev.set_setting(dev.d("MACDISK_KEY_NTP_HOST"), "pool ntp.org")),
                ("SET port 0", dev.set_setting(dev.d("MACDISK_KEY_NTP_PORT"), "0")),
                ("SET port 65536", dev.set_setting(dev.d("MACDISK_KEY_NTP_PORT"), "65536")),
                ("SET a 16-byte option", dev.set_setting(dev.d("MACDISK_KEYS") - dev.d("MACDISK_OPTIONS"),
                                                         "x" * dev.d("MACDISK_OPTION_SIZE"))),
            ]
            if files:
                refusals.append(("SET FOLDER to a file", dev.set_setting(
                    dev.d("MACDISK_KEY_FOLDER"), child(top, files[0]["name"]))))
            bad = [f"{label}: {result_name(dev, r)}" for label, r in refusals if r != param]
            after = dev.get_settings()
            same = after[1:] == (settings, slot)
            check("REFUSE", not bad and same,
                  ("; ".join(bad) or f"{len(refusals)} refusals, all MACDISK_ERR_PARAM")
                  + ("" if same else "; the settings CHANGED"))

            if args.set:
                fd1, fd0 = dev.d("MACDISK_KEY_FD1"), 0
                image = next((e for e in files if e["bytes"] >= 2 * SECTOR), None)
                if image is None:
                    check("SET", False, f"needs an image file in {top}")
                else:
                    path = child(top, image["name"])
                    rw = image["flags"] & dev.d("MACDISK_ENTRY_RW")
                    before = dev.unit(1)
                    got = dev.set_setting(fd1, path)
                    now = dev.unit(1)
                    readonly = bool(now["flags"] & dev.d("MACDISK_FLAG_READONLY"))
                    stored = dev.get_settings()[1][fd1]
                    ok = (got == ok_code and stored == path and now["changes"] != before["changes"]
                          and now["flags"] & dev.d("MACDISK_FLAG_INSERTED")
                          and (rw or readonly))
                    check("SET FD1", ok, f"{path}: {result_name(dev, got)}, unit {now}, "
                          f"{'named *.rw' if rw else 'read-only, not named *.rw'}")

                    got = dev.set_setting(fd1, settings[fd1])
                    swd.send_protocol(elf, dev.d("CHANDLER_ST_HELLO"), [])  # as at a boot
                    time.sleep(1.0)
                    back = dev.unit(1)
                    same = {k: back[k] for k in ("flags", "sectors", "format")} == \
                        {k: before[k] for k in ("flags", "sectors", "format")}
                    check("SET FD1 back", got == ok_code and same,
                          f"{settings[fd1]!r}: {result_name(dev, got)}, after a hello {back}")

                    nxt = slot % dev.d("MACDISK_FD0_SLOTS") + 1
                    before = dev.unit(0)
                    got = dev.set_setting(nxt, path)
                    alone = dev.unit(0) == before
                    swd.send_protocol(elf, dev.d("CHANDLER_ST_HELLO"), [])  # a Mac session
                    time.sleep(1.0)
                    ejected = dev.command("MACDISK_CMD_EJECT", fd0)
                    swd.cmd_select(argparse.Namespace(press="short", hold_ms=None, force=False))
                    time.sleep(1.5)
                    stepped = dev.unit(0)
                    _, _, now_slot = dev.get_settings()
                    ok = (got == ok_code and alone and ejected == ok_code and now_slot == nxt
                          and stepped["flags"] & dev.d("MACDISK_FLAG_INSERTED")
                          and stepped["sectors"] == now["sectors"])
                    check("SET and SELECT FD0", ok,
                          f"slot {nxt} {result_name(dev, got)}, FD0 "
                          f"{'untouched' if alone else 'CHANGED'}; after a hello, an eject and "
                          f"SELECT: slot {now_slot}, FD0 {stepped}")

                bad = []
                for key, value in enumerate(settings):
                    if key != dev.d("MACDISK_KEY_FOLDER"):
                        got = dev.set_setting(key, value)
                        if got != ok_code:
                            bad.append(f"key {key}: {result_name(dev, got)}")
                swd.chip_reset()
                after: tuple = (None,)
                end = time.monotonic() + 40
                while time.monotonic() < end:
                    time.sleep(2)
                    try:
                        # An app command nobody handles: acknowledged once the
                        # main loop runs, after the window is set up.
                        swd.mailbox_request(elf, swd.KIND_APP, 0xFFFF, [])
                        # A Mac ROM on the ST says hello and asks for the
                        # settings as soon as the RP is back, in the same
                        # buffer: read until two answers agree.
                        previous: tuple = (None,)
                        for _ in range(5):
                            after = dev.get_settings()
                            if after == previous:
                                break
                            previous = after
                            time.sleep(1)
                        break
                    except swd.SwdError:
                        pass
                same = after[1:] == (settings, 1)
                check("SET back", not bad and same,
                      ("; ".join(bad) or "every setting put back")
                      + (", the RP restarted: GET as before, slot 1" if same
                         else f"; after the restart GET answers {after}"))

        busy = dev.max_busy_us()
        check("BUSY", True, f"slowest command answered in {busy} us", max_busy_us=busy)
    except (swd.SwdError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    failed = [r for r in results if not r["ok"]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    os.makedirs(os.path.join(swd.HERE, "logs"), exist_ok=True)
    report = os.path.join(swd.HERE, "logs", time.strftime("macdisk-harness-%Y%m%d-%H%M%S.json"))
    with open(report, "w") as f:
        json.dump({"seed": args.seed, "results": results}, f, indent=2)
    print(f"report: {report}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
