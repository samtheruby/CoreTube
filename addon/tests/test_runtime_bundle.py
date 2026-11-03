# SPDX-License-Identifier: BSD-3-Clause
import hashlib
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "resources" / "lib")
)
from android_runtime_bundle import validate_bundle


class BundleTests(unittest.TestCase):
    def test_integrity_paths_and_links(self):
        files = {"system/bin/linker": b"linker", "vendor/lib/libwvhidl.so": b"library"}
        manifest = {
            name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in files.items()
        }
        files["runtime-manifest.json"] = json.dumps(manifest).encode()
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "runtime.tar.gz"

            def write(entries):
                with tarfile.open(path, "w:gz") as archive:
                    for name, data in entries.items():
                        entry = tarfile.TarInfo(name)
                        entry.size = len(data)
                        archive.addfile(entry, io.BytesIO(data))

            write(files)
            self.assertEqual(validate_bundle(path), manifest)
            write(dict(files, **{"vendor/lib/libwvhidl.so": b"tampered"}))
            with self.assertRaises(ValueError):
                validate_bundle(path)
            for name in (
                "../outside",
                "/system/lib/libc.so",
                "data/vendor/mediadrm/cert.bin",
            ):
                write(dict(files, **{name: b"x"}))
                with self.subTest(name=name), self.assertRaises(ValueError):
                    validate_bundle(path)
            write(files)
            with tarfile.open(path, "w:gz") as archive:
                entry = tarfile.TarInfo("system/bin/linker")
                entry.type = tarfile.SYMTYPE
                entry.linkname = "/outside"
                archive.addfile(entry)
            with self.assertRaises(ValueError):
                validate_bundle(path)
