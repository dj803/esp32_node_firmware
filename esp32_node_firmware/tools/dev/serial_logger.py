#!/usr/bin/env python3
"""Continuous no-reset serial logger.

Usage: python serial_logger.py <port> <output_file>

Opens the COM port with DTR=False / RTS=False pre-set so the chip is
not reset on connect (per CLAUDE.md "Capturing fresh-device first
boot" technique). Streams everything received to the output file with
millisecond timestamps prefixed per line.

Stop with SIGTERM / SIGINT (the kill -TERM <pid> done by the orchestrating
shell). The output file is line-buffered so partial writes are flushed
on every newline.
"""
import datetime
import sys

import serial


def main(port: str, out_path: str) -> int:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False

    with open(out_path, "a", encoding="utf-8", buffering=1) as f:
        f.write(
            f"# logger started {datetime.datetime.now().isoformat(timespec='seconds')} "
            f"port={port} baud=115200 (no-reset open)\n"
        )
        buf = bytearray()
        try:
            while True:
                chunk = s.read(2048)
                if chunk:
                    buf.extend(chunk)
                    while b"\n" in buf:
                        line, _, buf = buf.partition(b"\n")
                        ts = datetime.datetime.now().isoformat(timespec="milliseconds")
                        try:
                            txt = line.decode("utf-8", errors="replace").rstrip("\r")
                        except Exception:
                            txt = repr(line)
                        f.write(f"[{ts}] {txt}\n")
        except KeyboardInterrupt:
            f.write(
                f"# logger stopped {datetime.datetime.now().isoformat(timespec='seconds')}\n"
            )
        finally:
            s.close()
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.stderr.write("usage: serial_logger.py <port> <output_file>\n")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
