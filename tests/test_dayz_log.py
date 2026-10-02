"""Fixture regressions for shell-safe, bounded launch and command checks."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "dayz_log.py"


class LogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="dayz-log-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.log = self.root / "player's $log.txt"
        self.marker = self.root / "marker"
        self.marker.write_text("0\n")

    def run_helper(self, mode: str, *options: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 - fixed Python helper and fixture argv, no shell.
            [
                sys.executable,
                str(SCRIPT),
                mode,
                "--log",
                str(self.log),
                "--profile",
                str(self.root),
                "--marker",
                str(self.marker),
                *options,
            ],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

    def test_literal_shell_text_and_single_result_finish_promptly(self) -> None:
        text = "print player's $(touch SENTINEL); 'text' -> printed"
        self.log.write_text(text + "\n")
        started = time.monotonic()
        result = self.run_helper("command", "--timeout", "3", "--literal", text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), text)
        self.assertLess(time.monotonic() - started, 2)
        self.assertFalse((self.root / "SENTINEL").exists())

    def test_missing_log_is_not_success(self) -> None:
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_empty_log_is_not_success(self) -> None:
        self.log.touch()
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_large_fatal_log_is_not_success(self) -> None:
        self.log.write_text("[x] Fatal exception\n" + "healthy line\n" * 100000)
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_nonworld_launch_is_not_success(self) -> None:
        self.log.write_text("Proxy initialized\n")
        self.assertEqual(self.run_helper("check", "--require-world").returncode, 1)

    def test_world_launch_succeeds(self) -> None:
        self.log.write_text("Alternating eye camera verified\n")
        self.assertEqual(self.run_helper("check", "--require-world").returncode, 0)

    def test_script_error_is_not_skipped_by_proxy_offset(self) -> None:
        self.log.write_text("older proxy output\n" * 100 + "Proxy initialized\n")
        (self.root / "script_new.log").write_text("SCRIPT (E): Cannot compile\n")
        result = self.run_helper("wait-launch", "--since", "100", "--timeout", "2")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("SCRIPT (E)", result.stdout)
        self.assertEqual(self.run_helper("check", "--since", "100").returncode, 1)

    def test_total_timeout_is_not_doubled(self) -> None:
        self.log.write_text("Proxy initialized\n")
        started = time.monotonic()
        result = self.run_helper("wait-launch", "--timeout", "0.2")
        self.assertEqual(result.returncode, 1)
        self.assertLess(time.monotonic() - started, 1.5)

    def test_command_timeout(self) -> None:
        self.log.write_text("unrelated\n")
        result = self.run_helper("command", "--literal", "wanted", "--timeout", "0.1")
        self.assertEqual(result.returncode, 1)

    def test_followers_are_reaped_after_success_and_timeout(self) -> None:
        spec = importlib.util.spec_from_file_location("dayz_log_tested", SCRIPT)
        if spec is None or spec.loader is None:
            self.fail("Cannot load the log helper under test.")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.log.write_text("matched\n")
        created: list[subprocess.Popen[bytes]] = []
        real_popen = subprocess.Popen

        def tracking_popen(
            command: list[str], *, stdout: int
        ) -> subprocess.Popen[bytes]:
            process = real_popen(command, stdout=stdout)
            created.append(process)
            return process

        from unittest.mock import patch

        with patch.object(module.subprocess, "Popen", tracking_popen):
            self.assertEqual(
                module.follow([(self.log, 0)], 1, lambda line: line == "matched"),
                "matched",
            )
            self.assertIsNone(module.follow([(self.log, 0)], 0.1, lambda _line: False))
        self.assertEqual(len(created), 2)
        self.assertTrue(all(process.poll() is not None for process in created))


if __name__ == "__main__":
    unittest.main()
