#!/usr/bin/env python3
"""Check the Mac disk commands (rp/src/macdisk.c) on the device.

The firmware mounts FD0.*, FD1.* and HD0.* from its app folder at power-on.
Give the host's copies of the same files with --fd0, --fd1 and --hd0: every
case compares what the 68000 would read in the cartridge window with them.

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
  BUSY    the slowest command the RP answered since it booted
          (chandlerMaxBusyUs): the Mac ROM's timeout must cover it

The commands go through the devhooks mailbox, as if the ST had sent them, so
it needs a debug build and the Debug Probe, and no ST.

    python3 tools/dev/macdisk_harness.py --fd0 System.image --hd0 HD0.hda
    python3 tools/dev/macdisk_harness.py --hd0 HD0.hda --write --full --pram

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

    def max_busy_us(self) -> int:
        addr = swd.elf_symbols(self.elf, "chandlerMaxBusyUs")["chandlerMaxBusyUs"][0]
        return struct.unpack("<I", swd.read_memory(addr, 4))[0]


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
