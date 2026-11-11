# SPDX-License-Identifier: BSD-3-Clause
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "resources" / "lib")
)
from android_runtime_bundle import validate_bundle


def write_zip(path, entries, mode=0o100644):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            entry = zipfile.ZipInfo(name)
            entry.external_attr = mode << 16
            archive.writestr(entry, data)


def bundle(files):
    manifest = {
        name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in files.items()
    }
    return manifest, dict(files, **{"runtime-manifest.json": json.dumps(manifest)})


class BundleTests(unittest.TestCase):
    def test_integrity_paths_and_links(self):
        manifest, files = bundle(
            {"system/bin/linker": b"linker", "vendor/lib/libwvhidl.so": b"library"}
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "coretube-widevine.zip"
            write_zip(path, files)
            self.assertEqual(validate_bundle(path), manifest)
            write_zip(path, dict(files, **{"vendor/lib/libwvhidl.so": b"tampered"}))
            with self.assertRaises(ValueError):
                validate_bundle(path)
            for name in (
                "../outside",
                "/system/lib/libc.so",
                "data/vendor/mediadrm/cert.bin",
            ):
                write_zip(path, dict(files, **{name: b"x"}))
                with self.subTest(name=name), self.assertRaises(ValueError):
                    validate_bundle(path)
            write_zip(path, files, mode=0o120777)
            with self.assertRaises(ValueError):
                validate_bundle(path)

    def test_folder_entries_are_allowed(self):
        manifest, files = bundle(
            {"system/bin/linker": b"linker", "vendor/lib/libwvhidl.so": b"library"}
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "coretube-widevine.zip"
            write_zip(path, files)
            with zipfile.ZipFile(path, "a") as archive:
                archive.writestr("system/", b"")
            self.assertEqual(validate_bundle(path), manifest)

    def test_not_a_zip_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "coretube-widevine.zip"
            path.write_bytes(b"not a zip")
            with self.assertRaises(ValueError):
                validate_bundle(path)


class LibrariesOnlyTests(unittest.TestCase):
    def test_rejects_non_library_files_even_when_listed(self):
        # A profile or key file must never ship, even with a matching manifest entry.
        for extra in ("vendor/lib/ay64.dat", "system/lib/vndk-28/cert.bin"):
            _, files = bundle(
                {
                    "system/bin/linker": b"linker",
                    "vendor/lib/libwvhidl.so": b"library",
                    extra: b"secret",
                }
            )
            with tempfile.TemporaryDirectory() as temporary:
                path = pathlib.Path(temporary) / "coretube-widevine.zip"
                write_zip(path, files)
                with self.subTest(extra=extra), self.assertRaises(ValueError):
                    validate_bundle(path)
