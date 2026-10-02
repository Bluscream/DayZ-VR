"""Local-server launch/setup tests with a fake podman and tiny source installation."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
MOCK_PODMAN = """#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
with Path(os.environ["MOCK_TRACE"]).open("a") as stream:
    stream.write(json.dumps(sys.argv[1:]) + "\\n")
args = sys.argv[1:]
if args[:2] == ["container", "exists"]:
    raise SystemExit(0 if os.environ.get("MOCK_EXISTS") == "1" else 1)
if args and args[0] == "inspect":
    print(os.environ.get("MOCK_RUNNING", "false"))
"""


class ServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="dayz-server-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.scripts = self.root / "scripts"
        self.scripts.mkdir()
        self.script = self.scripts / "local-server.sh"
        shutil.copyfile(PROJECT / "scripts" / self.script.name, self.script)
        self.server = self.root / "build" / "local-server"
        self.server.mkdir(parents=True)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "DayZServer").write_bytes(b"fixture executable")
        (self.source / "steamclient.so").write_bytes(b"fixture loader")
        self.eggs = self.root / "eggs"
        self.eggs.mkdir()
        (self.eggs / "patch_be.pl").write_text("exit 0;\n")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "podman").write_text(MOCK_PODMAN)
        (self.bin / "podman").chmod(0o755)
        self.trace = self.root / "trace.jsonl"
        self.env = os.environ | {
            "PATH": f"{self.bin}:{os.environ['PATH']}",
            "DAYZ_SERVER_SRC": str(self.source),
            "EGGS_DIR": str(self.eggs),
            "MOCK_TRACE": str(self.trace),
            "MOCK_EXISTS": "0",
            "MOCK_RUNNING": "false",
            "SERVER_PORT": "2302",
            "SERVER_QUERY_PORT": "2305",
        }

    def invoke(self, command: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 - copied local script with fake podman and fixture paths.
            ["/bin/bash", str(self.script), command],
            env=self.env,
            cwd=self.root,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_setup_refuses_running_container_before_writing(self) -> None:
        self.env.update(MOCK_EXISTS="1", MOCK_RUNNING="true")
        result = self.invoke("setup")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("refusing to overwrite", result.stderr)
        self.assertFalse((self.server / "DayZServer").exists())

    def test_setup_preserves_deployed_mod_and_profile(self) -> None:
        for relative in [
            "@DayZVR_Server/addons/fixture.pbo",
            "serverprofile/save",
            ".steam/keep",
        ]:
            target = self.server / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("keep")
        result = self.invoke("setup")
        self.assertEqual(result.returncode, 0, result.stderr)
        for relative in [
            "@DayZVR_Server/addons/fixture.pbo",
            "serverprofile/save",
            ".steam/keep",
        ]:
            self.assertEqual((self.server / relative).read_text(), "keep")
        self.assertIn(
            "steamQueryPort = 2305;", (self.server / "serverDZ.cfg").read_text()
        )

    def test_start_publishes_only_loopback_ports(self) -> None:
        (self.server / "DayZServer").write_text("fixture")
        (self.server / "serverDZ.cfg").write_text("fixture")
        result = self.invoke("start")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in self.trace.read_text().splitlines()]
        run = next(call for call in calls if call[0] == "run")
        self.assertEqual(run[run.index("--network") + 1], "bridge")
        published = [run[index + 1] for index, arg in enumerate(run) if arg == "-p"]
        self.assertEqual(
            published, ["127.0.0.1:2302-2304:2302-2304/udp", "127.0.0.1:2305:2305/udp"]
        )

    def test_invalid_port_is_rejected_without_invoking_container(self) -> None:
        self.env["SERVER_PORT"] = "2302; echo unsafe"
        result = self.invoke("start")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.trace.exists())


if __name__ == "__main__":
    unittest.main()
