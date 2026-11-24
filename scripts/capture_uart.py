#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture the sdram_benchmark console output to a log (and optional CSV).

The port is opened first, then the board is flashed (or reset) so the
complete boot output is recorded. Capture stops at the final
"sdram_benchmark PASSED|FAILED" line or after --timeout seconds.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import time

import serial

END_RE = re.compile(r"sdram_benchmark (PASSED|FAILED)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0", help="console tty (uart0)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=180.0, help="seconds")
    parser.add_argument("--log", required=True, type=pathlib.Path, help="output log file")
    parser.add_argument("--csv", type=pathlib.Path,
                        help="write RESULT lines (and CONC/SUST lines) as CSV")
    parser.add_argument("--flash", metavar="BUILD_DIR", help="run 'west flash -d BUILD_DIR'")
    parser.add_argument("--jlink-serial",
                        help="J-Link emulator serial (west -r jlink --dev-id), e.g. the "
                        "on-board probe when several are connected")
    args = parser.parse_args()

    lines = []
    status = None

    with serial.Serial(args.port, args.baud, timeout=0.5) as ser:
        ser.reset_input_buffer()

        if args.flash:
            cmd = ["west", "flash", "-d", args.flash]
            if args.jlink_serial:
                cmd += ["--", "-r", "jlink", "--dev-id", args.jlink_serial]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

        deadline = time.monotonic() + args.timeout
        pending = b""
        while time.monotonic() < deadline and status is None:
            pending += ser.read(4096)
            *complete, pending = pending.split(b"\n")
            for raw in complete:
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                print(line, flush=True)
                lines.append(line)
                match = END_RE.search(line)
                if match:
                    status = match.group(1)

    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text("\n".join(lines) + "\n")

    if args.csv:
        # RESULT lines go to the given file; CONC/SUST/SUSTSUM lines, if any,
        # to <stem>_conc.csv, <stem>_sust.csv and <stem>_sustsum.csv
        for prefix, path in (("RESULT", args.csv),
                             ("CONC", args.csv.with_name(args.csv.stem + "_conc.csv")),
                             ("SUST", args.csv.with_name(args.csv.stem + "_sust.csv")),
                             ("SUSTSUM", args.csv.with_name(args.csv.stem + "_sustsum.csv"))):
            rows = [ln[len(prefix) + 1:] for ln in lines if ln.startswith(prefix + ",")]
            if rows:
                path.write_text("\n".join(rows) + "\n")

    if status is None:
        print(f"timeout after {args.timeout:.0f} s", file=sys.stderr)
        return 2

    return 0 if status == "PASSED" else 1


if __name__ == "__main__":
    sys.exit(main())
