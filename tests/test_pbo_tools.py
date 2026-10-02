"""PBO extraction regressions using disposable archives and destination trees."""

from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
CPRS = 0x43707273


def archive(entries: list[tuple[str, int, int, bytes]]) -> bytes:
    headers = bytearray()
    payload = bytearray()
    for name, method, original, data in entries:
        headers += (
            name.encode()
            + b"\0"
            + struct.pack("<IIIII", method, original, 0, 0, len(data))
        )
        payload += data
    return bytes(headers + b"\0" + bytes(20) + payload)


class PboTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="dayz-pbo-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.pbo = self.root / "fixture.pbo"
        self.output = self.root / "out"

    def extract(self, content: bytes) -> subprocess.CompletedProcess[str]:
        self.pbo.write_bytes(content)
        return subprocess.run(  # noqa: S603 - fixed Python helper with disposable fixture paths.
            [
                sys.executable,
                str(SCRIPTS / "unpack-pbo.py"),
                str(self.pbo),
                str(self.output),
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )

    def test_round_trip_builder_and_extractor(self) -> None:
        source = self.root / "source"
        (source / "scripts").mkdir(parents=True)
        (source / "scripts" / "test.c").write_text("void main() {}\n")
        (source / "$PROPERTIES$").write_text("old metadata\n")
        built = subprocess.run(  # noqa: S603 - fixed Python helper with disposable fixture paths.
            [
                sys.executable,
                str(SCRIPTS / "build-pbo.py"),
                str(source),
                str(self.pbo),
                "--prefix",
                "DayZVR",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        self.assertEqual(built.returncode, 0, built.stderr)
        result = self.extract(self.pbo.read_bytes())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.output / "scripts" / "test.c").read_text(), "void main() {}\n"
        )
        self.assertEqual((self.output / "$PBOPREFIX$").read_text(), "DayZVR\n")

    def test_reject_unsafe_paths_before_any_output(self) -> None:
        for name in [
            "../escaped",
            "nested/../../escaped",
            "/absolute",
            "C:\\drive",
            "./relative",
            "a//b",
        ]:
            with self.subTest(name=name):
                result = self.extract(
                    archive([("safe", 0, 1, b"s"), (name, 0, 1, b"x")])
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_reject_directory_symlink_escape(self) -> None:
        outside = self.root / "outside"
        outside.mkdir()
        sentinel = outside / "sentinel"
        sentinel.write_text("keep")
        self.output.mkdir()
        (self.output / "link").symlink_to(outside, target_is_directory=True)
        result = self.extract(archive([("link/sentinel", 0, 4, b"lost")]))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_reject_existing_file_symlink(self) -> None:
        sentinel = self.root / "sentinel"
        sentinel.write_text("keep")
        self.output.mkdir()
        (self.output / "link").symlink_to(sentinel)
        result = self.extract(archive([("link", 0, 4, b"lost")]))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_reject_metadata_symlink(self) -> None:
        sentinel = self.root / "sentinel"
        sentinel.write_text("keep")
        self.output.mkdir()
        (self.output / "$PBOPREFIX$").symlink_to(sentinel)
        header = (
            b"\0" + struct.pack("<IIIII", 0x56657273, 0, 0, 0, 0) + b"prefix\0new\0\0"
        )
        result = self.extract(header + archive([]))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")

    def test_reject_duplicate_and_file_directory_collisions(self) -> None:
        for names in [("File", "file"), ("dir", "dir/file"), ("a\\b", "a/b")]:
            with self.subTest(names=names):
                result = self.extract(archive([(name, 0, 1, b"x") for name in names]))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_lzss_literals_and_overlapping_reference(self) -> None:
        data = b"\x01A\x01\x00" + struct.pack("<I", 4 * ord("A"))
        result = self.extract(archive([("decoded", CPRS, 4, data)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "decoded").read_bytes(), b"AAAA")

    def test_lzss_leading_spaces(self) -> None:
        data = b"\x00\x01\x00" + struct.pack("<I", 3 * ord(" "))
        result = self.extract(archive([("decoded", CPRS, 3, data)]))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "decoded").read_bytes(), b"   ")

    def test_reject_corrupt_lzss_without_raw_fallback(self) -> None:
        for data in [b"\0\0\0", b"\xff", b"\x01A\x01\x00", b"\x01A\x01\x00" + bytes(4)]:
            with self.subTest(data=data):
                result = self.extract(
                    archive([("safe", 0, 1, b"s"), ("decoded", CPRS, 4, data)])
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertFalse(self.output.exists())

    def test_reject_unknown_compression(self) -> None:
        result = self.extract(archive([("unknown", 123, 1, b"x")]))
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.output.exists())

    def test_reject_truncated_payload(self) -> None:
        result = self.extract(archive([("truncated", 0, 5, b"12345")])[:-1])
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
