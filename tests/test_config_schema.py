"""dayz_openxr.schema.json must describe exactly the keys in dayz_openxr.ini and agree
with the ranges the native tunable tables enforce."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INI = ROOT / "dayz_openxr.ini"
SCHEMA = ROOT / "dayz_openxr.schema.json"
HOST_ROWS = re.compile(r'\{\{"([a-z]+\.[a-z0-9_]+)",\s*([-0-9.]+)f,\s*([-0-9.]+)f,\s*([-0-9.]+)f,\s*(true|false)\}')
PROBE_ROWS = re.compile(r'\{"([a-z]+\.[a-z0-9_]+)",\s*TunableKind::(\w+),\s*[^,]+,\s*([-0-9.]+)f,\s*([-0-9.]+)f\}')
KNOWN_TYPES = {"bool", "int", "float", "enum", "text"}
# Native tunables that exist only at runtime (diagnostics) and are deliberately not ini keys.
LIVE_ONLY = {"stereo.double_debug_yaw"}


def ini_keys() -> dict[str, str]:
    keys: dict[str, str] = {}
    section = ""
    for line in INI.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
        elif "=" in stripped and not stripped.startswith(("#", ";")):
            key, value = stripped.split("=", 1)
            keys[f"{section}.{key.strip()}"] = value.strip()
    return keys


def schema_keys() -> dict[str, dict[str, object]]:
    data = json.loads(SCHEMA.read_text(encoding="utf-8"))
    return {
        f"{section}.{key}": info
        for section, body in data["sections"].items()
        for key, info in body["keys"].items()
    }


def native_ranges() -> dict[str, tuple[float, float, bool]]:
    ranges: dict[str, tuple[float, float, bool]] = {}
    host = (ROOT / "common" / "openxr_host.cpp").read_text(encoding="utf-8")
    for name, _default, low, high, boolean in HOST_ROWS.findall(host):
        ranges[name] = (float(low), float(high), boolean == "true")
    probe = (ROOT / "common" / "dayz_runtime_probe.cpp").read_text(encoding="utf-8")
    for name, kind, low, high in PROBE_ROWS.findall(probe):
        ranges[name] = (float(low), float(high), kind == "Bool")
    return ranges


class SchemaTests(unittest.TestCase):
    def test_schema_matches_ini_keys(self) -> None:
        ini = set(ini_keys())
        schema = set(schema_keys())
        self.assertEqual(sorted(ini - schema), [], "ini keys missing from the schema")
        self.assertEqual(sorted(schema - ini), [], "schema keys missing from the ini")

    def test_every_key_has_title_description_and_type(self) -> None:
        for name, info in schema_keys().items():
            with self.subTest(name=name):
                self.assertTrue(str(info.get("title", "")).strip(), "title")
                self.assertTrue(str(info.get("description", "")).strip(), "description")
                self.assertIn(info.get("type"), KNOWN_TYPES)
                if info.get("type") == "enum":
                    self.assertTrue(info.get("values"), "enum values")

    def test_sections_have_titles(self) -> None:
        data = json.loads(SCHEMA.read_text(encoding="utf-8"))
        for section, body in data["sections"].items():
            with self.subTest(section=section):
                self.assertTrue(str(body.get("title", "")).strip())

    def test_ranges_and_live_flags_match_native_tables(self) -> None:
        schema = schema_keys()
        native = native_ranges()
        # The host tunable table is parked (parked/README.md); only the probe rows remain.
        self.assertGreater(len(native), 15, "tunable table regexes found too few rows")
        for name, (low, high, boolean) in native.items():
            if name in LIVE_ONLY:
                continue
            with self.subTest(name=name):
                self.assertIn(name, schema)
                info = schema[name]
                self.assertTrue(info.get("live"), "native tunable must be marked live")
                if boolean:
                    self.assertEqual(info["type"], "bool")
                else:
                    self.assertIn(info["type"], {"int", "float"})
                    self.assertAlmostEqual(float(info["min"]), low, places=4)
                    self.assertAlmostEqual(float(info["max"]), high, places=4)
        for name, info in schema.items():
            if info.get("live") and name not in native:
                self.fail(f"{name} is marked live but has no native tunable row")

    def test_ini_values_fit_the_schema(self) -> None:
        schema = schema_keys()
        for name, value in ini_keys().items():
            info = schema[name]
            with self.subTest(name=name):
                kind = info["type"]
                if kind == "bool":
                    self.assertIn(value, {"true", "false"})
                elif kind == "enum":
                    self.assertIn(value, info["values"])
                elif kind in {"int", "float"}:
                    number = float(value)
                    if kind == "int":
                        self.assertEqual(number, int(float(value)))
                    if "min" in info:
                        self.assertGreaterEqual(number, float(info["min"]))
                    if "max" in info:
                        self.assertLessEqual(number, float(info["max"]))


if __name__ == "__main__":
    unittest.main()
