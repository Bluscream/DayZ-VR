#!/usr/bin/env python3
"""Unpack a Bohemia PBO (DayZ/Arma format) into a directory.

    scripts/unpack-pbo.py <pbo> <outdir> [--list] [--verbose]

Format (mirrors scripts/build-pbo.py): header entries of ``name\\0`` followed by
five little-endian uint32 (packing method, original size, reserved, timestamp,
data size). The first entry is normally an empty-name "Vers" product entry
(packing 0x56657273) followed by null-terminated key/value string pairs ended by
an empty string. The header ends with an empty-name entry of 20 zero bytes, then
the payloads follow in header order. Entries with packing 0x43707273 ("Cprs")
are BI LZSS compressed and are expanded here; unknown methods are written raw and
reported. Product properties (e.g. ``prefix``) are written to ``$PBOPREFIX$`` /
``$PROPERTIES$`` files in the output root.
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

PACKING_VERS = 0x56657273  # "Vers"
PACKING_CPRS = 0x43707273  # "Cprs"
PACKING_RAW = 0
HEADER_STRUCT = struct.Struct("<IIIII")


@dataclass
class Entry:
    name: str
    packing: int
    original_size: int
    reserved: int
    timestamp: int
    data_size: int


class PboError(Exception):
    pass


def read_cstring(data: bytes, offset: int) -> tuple[bytes, int]:
    end = data.find(b"\0", offset)
    if end < 0:
        raise PboError("unterminated string in header")
    return data[offset:end], end + 1


def parse_header(data: bytes) -> tuple[dict[str, str], list[Entry], int]:
    """Return (properties, entries, payload_offset)."""
    offset = 0
    properties: dict[str, str] = {}
    entries: list[Entry] = []
    while True:
        raw_name, offset = read_cstring(data, offset)
        if offset + HEADER_STRUCT.size > len(data):
            raise PboError("truncated header entry")
        packing, original, reserved, timestamp, size = HEADER_STRUCT.unpack_from(data, offset)
        offset += HEADER_STRUCT.size
        if raw_name == b"" and packing == PACKING_VERS:
            while True:
                key, offset = read_cstring(data, offset)
                if key == b"":
                    break
                value, offset = read_cstring(data, offset)
                properties[key.decode("latin-1")] = value.decode("latin-1")
            continue
        if raw_name == b"":
            # Terminator: 20 zero bytes (some packers leave garbage; accept any).
            break
        entries.append(Entry(raw_name.decode("latin-1"), packing, original, reserved, timestamp, size))
    return properties, entries, offset


def lzss_decompress(src: bytes, expected: int) -> bytes:
    """Bohemia LZSS: flag byte, 8 items; bit set = literal, clear = back-reference.

    Back-reference: two bytes b1, b2; rpos = i - ((b1 | ((b2 & 0xF0) << 4)) );
    length = (b2 & 0x0F) + 3. If rpos is before the start of the output the
    missing bytes are spaces (0x20). The stream is followed by a 4-byte
    checksum (sum of output bytes) which is validated when present.
    """
    out = bytearray()
    pos = 0
    n = len(src)
    while len(out) < expected:
        if pos >= n:
            raise PboError("LZSS stream ended early")
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= expected:
                break
            if flags & (1 << bit):
                if pos >= n:
                    raise PboError("LZSS literal past end")
                out.append(src[pos])
                pos += 1
            else:
                if pos + 1 >= n:
                    raise PboError("LZSS pointer past end")
                b1, b2 = src[pos], src[pos + 1]
                pos += 2
                rpos = len(out) - (b1 | ((b2 & 0xF0) << 4))
                rlen = (b2 & 0x0F) + 3
                for _ in range(rlen):
                    if len(out) >= expected:
                        break
                    if rpos < 0:
                        out.append(0x20)
                    else:
                        out.append(out[rpos])
                    rpos += 1
    if pos + 4 <= n:
        (checksum,) = struct.unpack_from("<I", src, pos)
        if checksum != sum(out) & 0xFFFFFFFF:
            raise PboError("LZSS checksum mismatch")
    return bytes(out)


def safe_target(outdir: Path, name: str) -> Path:
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    if not parts:
        raise PboError(f"unsafe entry name {name!r}")
    return outdir.joinpath(*parts)


def unpack(pbo: Path, outdir: Path, list_only: bool, verbose: bool) -> int:
    data = pbo.read_bytes()
    properties, entries, offset = parse_header(data)
    if list_only:
        for key, value in properties.items():
            print(f"property {key}={value}")
        for e in entries:
            method = {PACKING_RAW: "raw", PACKING_CPRS: "lzss"}.get(e.packing, f"0x{e.packing:08x}")
            print(f"{e.data_size:>10} {e.original_size:>10} {method:<10} {e.name}")
        print(f"{len(entries)} entries, payload at {offset}, file {len(data)} bytes")
        return 0

    outdir.mkdir(parents=True, exist_ok=True)
    if "prefix" in properties:
        (outdir / "$PBOPREFIX$").write_text(properties["prefix"] + "\n", encoding="latin-1")
    if properties:
        lines = "".join(f"{k}={v}\n" for k, v in properties.items())
        (outdir / "$PROPERTIES$").write_text(lines, encoding="latin-1")

    written = failed = compressed = unknown = 0
    for e in entries:
        blob = data[offset:offset + e.data_size]
        offset += e.data_size
        if len(blob) != e.data_size:
            print(f"error: truncated payload for {e.name}", file=sys.stderr)
            failed += 1
            continue
        if e.packing == PACKING_CPRS:
            compressed += 1
            try:
                blob = lzss_decompress(blob, e.original_size)
            except PboError as exc:
                print(f"error: {e.name}: {exc} (written raw)", file=sys.stderr)
                failed += 1
        elif e.packing != PACKING_RAW:
            unknown += 1
            print(f"warning: {e.name}: unknown packing 0x{e.packing:08x}, written raw", file=sys.stderr)
        target = safe_target(outdir, e.name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob)
        written += 1
        if verbose:
            print(target)
    remaining = len(data) - offset
    print(
        f"{pbo.name}: {written} files -> {outdir} "
        f"(compressed {compressed}, unknown packing {unknown}, errors {failed}, "
        f"trailing {remaining} bytes, prefix {properties.get('prefix', '-')})"
    )
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pbo", type=Path)
    parser.add_argument("outdir", type=Path)
    parser.add_argument("--list", action="store_true", help="list entries instead of extracting")
    parser.add_argument("--verbose", action="store_true", help="print each written path")
    args = parser.parse_args(argv)
    try:
        return unpack(args.pbo, args.outdir, args.list, args.verbose)
    except (PboError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
