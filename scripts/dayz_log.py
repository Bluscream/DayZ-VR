#!/usr/bin/env python3
"""Bounded log following and launch classification for the DayZ test rig."""

from __future__ import annotations

import argparse
import os
import re
import selectors
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from contextlib import ExitStack
from pathlib import Path

WORLD = re.compile(r"Alternating eye camera verified|Double world render frame 1 ")
FAILURE = re.compile(r"\] Fatal exception|SCRIPT\s*\(E\)|Can.t compile")


def follow(
    sources: Sequence[tuple[Path, int]],
    timeout: float,
    matches: Callable[[str], bool],
) -> str | None:
    """Use GNU tail's filesystem notifications; always reap every owned child."""
    if timeout <= 0:
        return None
    tail = shutil.which("tail")
    if tail is None:
        raise FileNotFoundError("GNU tail is required to follow the game logs.")
    deadline = time.monotonic() + timeout
    processes: list[subprocess.Popen[bytes]] = []
    pending: dict[int, bytes] = {}
    with ExitStack() as stack, selectors.DefaultSelector() as selector:
        try:
            for path, since in sources:
                process = stack.enter_context(
                    subprocess.Popen(  # noqa: S603 - fixed executable; paths follow -- and never enter shell code.
                        [tail, "-q", "-n", f"+{since + 1}", "-F", "--", str(path)],
                        stdout=subprocess.PIPE,
                    )
                )
                processes.append(process)
                if process.stdout is None:
                    raise RuntimeError("Log follower has no output pipe.")
                descriptor = process.stdout.fileno()
                pending[descriptor] = b""
                selector.register(descriptor, selectors.EVENT_READ)
            while (remaining := deadline - time.monotonic()) > 0:
                events = selector.select(remaining)
                if not events:
                    return None
                for key, _ in events:
                    descriptor = key.fd
                    chunk = os.read(descriptor, 65536)
                    if not chunk:
                        selector.unregister(descriptor)
                        if not selector.get_map():
                            return None
                        continue
                    records = (pending[descriptor] + chunk).split(b"\n")
                    pending[descriptor] = records.pop()
                    for record in records:
                        line = record.decode("utf-8", errors="replace")
                        if matches(line):
                            return line
            return None
        finally:
            for process in processes:
                process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)


def current_scripts(profile: Path, marker: Path) -> list[Path]:
    """Old script errors must not poison a new run's result."""
    if not marker.is_file():
        return []
    started = marker.stat().st_mtime_ns
    return sorted(
        path
        for path in profile.glob("script_*.log")
        if path.stat().st_mtime_ns >= started
    )


def wait_launch(
    log: Path, since: int, timeout: float, profile: Path, marker: Path
) -> bool:
    deadline = time.monotonic() + timeout
    first = follow([(log, since)], timeout, lambda line: bool(line))
    if first is None:
        print("No proxy output before the launch timeout.")
        return False
    # The proxy starts before the script VM. Final classification also checks logs
    # created after this discovery, so a late compile error cannot produce success.
    sources = [
        (log, since),
        *((script, 0) for script in current_scripts(profile, marker)),
    ]
    found = follow(
        sources,
        max(0.0, deadline - time.monotonic()),
        lambda line: bool(WORLD.search(line) or FAILURE.search(line)),
    )
    if found is None:
        print("No in-world or failure marker before the launch timeout.")
        return False
    print(found)
    return not bool(FAILURE.search(found))


def classify(
    log: Path, since: int, profile: Path, marker: Path, require_world: bool
) -> tuple[bool, str]:
    try:
        if marker.is_file() and log.stat().st_mtime_ns < marker.stat().st_mtime_ns:
            return False, "Proxy log predates this launch."
        lines = log.read_text(errors="replace").splitlines()[since:]
    except OSError as error:
        return False, f"Proxy log is unavailable: {error}"
    if not any(line.strip() for line in lines):
        return False, "No proxy output for this launch."
    if any(FAILURE.search(line) for line in lines):
        return False, "Fatal exception or script error in the proxy log."
    for script in current_scripts(profile, marker):
        if FAILURE.search(script.read_text(errors="replace")):
            return False, f"Script error in {script.name}."
    in_world = any(WORLD.search(line) for line in lines)
    if require_world and not in_world:
        return False, "Launch did not reach an in-world verification marker."
    return (
        True,
        "In-world marker verified; no fatal/script error."
        if in_world
        else "Proxy log present; no fatal/script error (world state not verified).",
    )


def nonnegative(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("Must be nonnegative.")
    return number


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["command", "wait-launch", "check"])
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--since", type=nonnegative, default=0)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--literal", default="")
    parser.add_argument("--profile", type=Path, default=Path())
    parser.add_argument("--marker", type=Path, default=Path("openxr-log-offset.txt"))
    parser.add_argument("--require-world", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout <= 0 or not args.timeout < float("inf"):
        parser.error("--timeout must be finite and positive")
    try:
        if args.mode == "command":
            found = follow(
                [(args.log, args.since)],
                args.timeout,
                lambda line: args.literal in line,
            )
            if found is None:
                print("No matching command result before the timeout.", file=sys.stderr)
                return 1
            print(found)
            return 0
        if args.mode == "wait-launch":
            return (
                0
                if wait_launch(
                    args.log, args.since, args.timeout, args.profile, args.marker
                )
                else 1
            )
        passed, reason = classify(
            args.log, args.since, args.profile, args.marker, args.require_world
        )
        print(reason)
        return 0 if passed else 1
    except OSError as error:
        print(f"Log inspection failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
