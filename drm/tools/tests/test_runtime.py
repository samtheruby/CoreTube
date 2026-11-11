# SPDX-License-Identifier: BSD-3-Clause
"""Runtime capture: dependency collection, bundle writing and the check command."""

import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

tools = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(tools))
import runtime_bundle
from runtime_bundle import collect_runtime, write_bundle

# Not in git: the Widevine zip is copied here by hand for builds (see README).
local_runtime = tools.parent / "runtime" / "coretube-widevine.zip"


def check(path):
    return subprocess.run(
        [sys.executable, "-I", str(tools / "runtime_bundle.py"), "check", str(path)],
        capture_output=True,
        text=True,
        check=False,  # the tests read the exit code
    )


class CollectTests(unittest.TestCase):
    def test_collects_transitive_dependencies_once_with_vendor_precedence(self):
        files = {
            "/system/bin/linker": b"linker",
            "/vendor/lib/libwvhidl.so": b"widevine",
            "/vendor/lib/libshared.so": b"vendor",
            "/system/lib/libshared.so": b"system",
            "/system/lib/libc.so": b"libc",
        }
        dependencies = {
            b"helper": ["libshared.so"],
            b"widevine": ["libshared.so"],
            b"vendor": ["libc.so"],
            b"libc": [],
            b"linker": [],
        }
        with mock.patch.object(
            runtime_bundle, "elf_dependencies", side_effect=dependencies.__getitem__
        ):
            result = collect_runtime(files.__getitem__, files.__contains__, b"helper")
        self.assertEqual(set(result), set(files) - {"/system/lib/libshared.so"})
        self.assertEqual(result["/vendor/lib/libshared.so"], b"vendor")

    def test_missing_dependency_and_path_traversal_fail(self):
        for name in ("missing.so", "../../data/cert.bin", "/data/private.so"):
            with (
                self.subTest(name=name),
                mock.patch.object(
                    runtime_bundle, "elf_dependencies", return_value=[name]
                ),
                self.assertRaises(ValueError),
            ):
                collect_runtime(
                    lambda path: b"elf",
                    lambda path: (
                        path in ("/system/bin/linker", "/vendor/lib/libwvhidl.so")
                    ),
                    b"helper",
                )


class CheckTests(unittest.TestCase):
    @unittest.skipUnless(local_runtime.is_file(), "no local coretube-widevine.zip")
    def test_local_runtime_passes(self):
        result = check(local_runtime)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("21 files", result.stdout)

    def test_written_bundle_passes_and_profile_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            good = pathlib.Path(temporary) / "good.zip"
            write_bundle(
                good, {"/system/bin/linker": b"l", "/vendor/lib/libwvhidl.so": b"w"}
            )
            self.assertEqual(check(good).returncode, 0)
            with zipfile.ZipFile(good) as archive:
                self.assertEqual(
                    archive.getinfo("system/bin/linker").external_attr >> 16, 0o100755
                )
            self.assertEqual(
                check(pathlib.Path(temporary) / "missing.zip").returncode, 1
            )
            bad = pathlib.Path(temporary) / "bad.zip"
            write_bundle(
                bad,
                {
                    "/system/bin/linker": b"l",
                    "/vendor/lib/libwvhidl.so": b"w",
                    "/vendor/lib/ay64.dat": b"profile",
                },
            )
            result = check(bad)
            self.assertEqual(result.returncode, 1)
            self.assertIn("runtime check failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
