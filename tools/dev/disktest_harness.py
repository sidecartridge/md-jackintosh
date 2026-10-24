#!/usr/bin/env python3
"""Run the disk self-test on the device (rp/src/devdisktest.c) and report it.

The self-test runs on the RP itself, in debug builds: it creates its own floppy
and hard disk images in SELFTEST/ under the app folder, mounts them in the
three units, reads and writes them through the Mac's commands, checks every
byte against the pattern it wrote, and mounts the card's own images again. It
never writes the card's own images. Nothing answers the ST while it runs.

Two ways to run it:

  - Over SWD (this script, Debug Probe attached): it sends the app command
    DEVHOOKS_APP_DISKTEST, follows devdisktestState until the run ends, and
    prints the report lines from the console log when `console.py watch` runs.
  - From the card, with no probe: create an empty SELFTEST.RUN in the app
    folder (/jackintosh by default) and restart the RP. The test runs at boot,
    deletes SELFTEST.RUN first, and writes SELFTEST.LOG next to it. Check a copy
    of that file with --from-card.

    python3 tools/dev/disktest_harness.py
    python3 tools/dev/disktest_harness.py --from-card /Volumes/SD/jackintosh/SELFTEST.LOG

Exits 0 when every check passed. Writes tools/dev/logs/disktest-harness-<time>.json.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import time

import swd

LOG = os.path.join(swd.HERE, "logs", "console.log")
STATE_SYMBOL = "devdisktestState"
# DevdisktestState in rp/src/include/devdisktest.h: 14 longs, then the first
# failure's text.
STATE_FORMAT = "<14I96s"
FIELDS = ("magic", "seq", "state", "checks", "failures", "elapsed_ms",
          "read32_count", "read32_max_us", "read32_total_us",
          "write2_count", "write2_max_us", "write2_total_us",
          "command_max_us", "cluster_bytes", "first_failure")
STATES = ("idle", "requested", "running", "done")


def read_state(elf: str, address: int) -> dict:
    raw = swd.read_memory(address, struct.calcsize(STATE_FORMAT))
    values = struct.unpack(STATE_FORMAT, raw)
    state = dict(zip(FIELDS, values))
    state["first_failure"] = state["first_failure"].split(b"\0")[0].decode(
        "latin-1")
    return state


def log_size() -> int:
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def log_lines_since(position: int) -> list[str]:
    """The self-test's lines that console.py logged after position."""
    if not os.path.exists(LOG):
        return []
    with open(LOG, "rb") as f:
        f.seek(position if os.path.getsize(LOG) >= position else 0)
        text = f.read().decode("utf-8", "replace")
    return [m.group(1) for m in re.finditer(r"disktest: (.*)", text)]


def summarize(lines: list[str]) -> tuple[int, int]:
    passed = sum(1 for ln in lines if ln.startswith("PASS "))
    failed = sum(1 for ln in lines if ln.startswith("FAIL "))
    return passed, failed


def write_report(report: dict) -> None:
    os.makedirs(os.path.join(swd.HERE, "logs"), exist_ok=True)
    path = os.path.join(swd.HERE, "logs",
                        time.strftime("disktest-harness-%Y%m%d-%H%M%S.json"))
    with open(path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"report: {path}")


def from_card(path: str) -> int:
    with open(path, encoding="latin-1") as f:
        lines = [ln.rstrip("\n") for ln in f]
    for ln in lines:
        print(f"  {ln}")
    passed, failed = summarize(lines)
    finished = any(re.match(r"(PASSED|FAILED): ", ln) for ln in lines)
    ok = finished and failed == 0 and passed > 0
    print(f"\n{passed} passed, {failed} failed"
          + ("" if finished else "; the run did not finish (no summary line)"))
    write_report({"source": path, "passed": passed, "failed": failed,
                  "finished": finished, "lines": lines})
    return 0 if ok else 1


def on_device(elf_arg: str | None, timeout: float) -> int:
    elf = swd.matching_elf(elf_arg)
    sym = swd.elf_symbols(elf, STATE_SYMBOL).get(STATE_SYMBOL)
    if not sym:
        raise swd.SwdError(f"{os.path.basename(elf)} has no {STATE_SYMBOL}: "
                           "a debug build is needed")
    address = sym[0]
    before = read_state(elf, address)
    if before["magic"] != 0x54534B44:
        raise swd.SwdError(f"{STATE_SYMBOL} has magic 0x{before['magic']:08x}")
    command = swd.include_defines()["DEVHOOKS_APP_DISKTEST"]
    position = log_size()
    if not swd.mailbox_request(elf, swd.KIND_APP, command, []):
        raise swd.SwdError("the firmware refused: a self-test is already pending")
    print(f"build {os.path.basename(elf)}: self-test run {before['seq'] + 1} started")

    deadline = time.monotonic() + timeout
    shown = -1
    state = before
    while time.monotonic() < deadline:
        try:
            state = read_state(elf, address)
        except swd.SwdError:
            time.sleep(1)  # a momentary debug-port drop
            continue
        if state["seq"] > before["seq"] and state["checks"] != shown:
            shown = state["checks"]
            print(f"  {STATES[state['state']] if state['state'] < 4 else state['state']}: "
                  f"{state['checks']} checks, {state['failures']} failed")
        if state["seq"] > before["seq"] and state["state"] == 3:
            break
        time.sleep(2)
    else:
        raise swd.SwdError(f"the self-test did not finish within {timeout:.0f} s")

    lines = log_lines_since(position)
    if lines:
        print()
        for ln in lines:
            print(f"  {ln}")
    else:
        print("  (no console log: run `console.py watch` to see each check)")

    def mean(total: str, count: str) -> int:
        return state[total] // state[count] if state[count] else 0

    print(f"\nREAD of 32 sectors: {state['read32_count']}, longest "
          f"{state['read32_max_us']} us, mean {mean('read32_total_us', 'read32_count')} us")
    print(f"WRITE of 2 sectors: {state['write2_count']}, longest "
          f"{state['write2_max_us']} us, mean {mean('write2_total_us', 'write2_count')} us")
    print(f"any command: longest {state['command_max_us']} us; card cluster "
          f"{state['cluster_bytes']} bytes; {state['elapsed_ms'] / 1000:.1f} s in all")
    print(f"\n{state['checks'] - state['failures']}/{state['checks']} checks passed"
          + (f"; first failure: {state['first_failure']}" if state["failures"] else ""))
    write_report({"build": os.path.basename(elf), "state": state, "lines": lines})
    return 0 if state["failures"] == 0 and state["checks"] > 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-card", metavar="LOG",
                    help="check a SELFTEST.LOG copied from the card instead")
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="seconds to wait for the run (default 900)")
    ap.add_argument("--elf")
    args = ap.parse_args()
    try:
        if args.from_card:
            return from_card(args.from_card)
        return on_device(args.elf, args.timeout)
    except (swd.SwdError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
