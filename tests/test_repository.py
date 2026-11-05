# SPDX-License-Identifier: BSD-3-Clause
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


class RepositoryTests(unittest.TestCase):
    def test_builds_repository_for_both_architectures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            assets, output = root / "assets", root / "site"
            assets.mkdir()
            checksums = []
            for arch, bits, machine in [("arm", 1, 40), ("aarch64", 2, 183)]:
                package = assets / f"browser.tizentube-0.5.4-{arch}.zip"
                header = bytearray(64)
                header[:6] = b"\x7fELF" + bytes([bits, 1])
                struct.pack_into("<H", header, 18, machine)
                with zipfile.ZipFile(package, "w") as z:
                    metadata = ET.parse(REPO / "addon/addon.xml").getroot()
                    metadata.set("version", "0.5.4")
                    extension = metadata.find("extension[@point='xbmc.addon.metadata']")
                    assert extension is not None
                    ET.SubElement(extension, "path").text = "obsolete/package.zip"
                    z.writestr("browser.tizentube/addon.xml", ET.tostring(metadata))
                    z.writestr("browser.tizentube/resources/icon.png", b"icon")
                    z.writestr("browser.tizentube/cobalt/cobalt", header)
                checksums.append(
                    f"{hashlib.sha256(package.read_bytes()).hexdigest()}  {package.name}\n"
                )
            (assets / "SHA256SUMS").write_text("".join(checksums))
            result = subprocess.run(
                [
                    sys.executable,
                    str(REPO / "tools/build_repository.py"),
                    str(assets),
                    str(output),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            for arch, platform in [("arm", "linux"), ("aarch64", "linux-aarch64")]:
                index = (output / arch / "addons.xml").read_bytes()
                self.assertEqual(
                    (output / arch / "addons.xml.sha256").read_text().strip(),
                    hashlib.sha256(index).hexdigest(),
                )
                addons = ET.fromstring(index)
                variants = addons.findall("addon[@id='browser.tizentube']")
                self.assertEqual(len(variants), 1)
                addon = variants[0]
                variant_metadata = addon.find("extension[@point='xbmc.addon.metadata']")
                assert variant_metadata is not None
                self.assertEqual(variant_metadata.findtext("platform"), platform)
                self.assertIsNone(variant_metadata.find("path"))
                relative = f"{arch}/browser.tizentube/browser.tizentube-0.5.4.zip"
                self.assertEqual(addon.attrib["version"], "0.5.4")
                self.assertEqual(
                    (output / relative).read_bytes(),
                    (assets / f"browser.tizentube-0.5.4-{arch}.zip").read_bytes(),
                )
                self.assertEqual(
                    (output / (relative + ".sha256")).read_text().strip(),
                    hashlib.sha256((output / relative).read_bytes()).hexdigest(),
                )
                self.assertEqual(
                    (output / relative)
                    .parent.joinpath("resources/icon.png")
                    .read_bytes(),
                    b"icon",
                )
            repository = addons.find("addon[@id='repository.coretube']")
            assert repository is not None
            installer = (
                output / f"repository.coretube-{repository.attrib['version']}.zip"
            )
            self.assertIn(
                f'href="{installer.name}"', (output / "index.html").read_text()
            )
            with zipfile.ZipFile(installer) as z:
                manifest = ET.fromstring(z.read("repository.coretube/addon.xml"))
            directories = manifest.findall(
                "extension[@point='xbmc.addon.repository']/dir"
            )
            self.assertEqual(len(directories), 2)
            for directory, arch, major in zip(
                directories, ["arm", "aarch64"], [21, 22]
            ):
                self.assertEqual(directory.attrib["minversion"], f"{major}.0.0")
                self.assertEqual(directory.attrib["maxversion"], f"{major}.999.999")
                base = f"https://samtheruby.github.io/CoreTube/{arch}/"
                self.assertEqual(directory.findtext("info"), base + "addons.xml")
                self.assertEqual(directory.findtext("datadir"), base)
                self.assertEqual(directory.findtext("hashes"), "sha256")
                checksum = directory.find("checksum")
                assert checksum is not None
                self.assertEqual(checksum.attrib["verify"], "sha256")
                self.assertEqual(
                    directory.findtext("checksum"), base + "addons.xml.sha256"
                )
            self.assertTrue((output / ".nojekyll").is_file())

    def test_rejects_modified_release_before_writing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "browser.tizentube-0.5.4-arm.zip"
            package.write_bytes(b"changed release")
            (root / "SHA256SUMS").write_text(f"{'0' * 64}  {package.name}\n")
            result = subprocess.run(
                [
                    sys.executable,
                    str(REPO / "tools/build_repository.py"),
                    str(root),
                    str(root / "site"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checksum mismatch", result.stderr)
            self.assertFalse((root / "site").exists())

    def test_rejects_wrong_architecture_before_writing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "browser.tizentube-0.5.4-arm.zip"
            with zipfile.ZipFile(package, "w") as z:
                metadata = ET.parse(REPO / "addon/addon.xml").getroot()
                metadata.set("version", "0.5.4")
                z.writestr("browser.tizentube/addon.xml", ET.tostring(metadata))
                z.writestr("browser.tizentube/cobalt/cobalt", b"not an ARM executable")
            (root / "SHA256SUMS").write_text(
                f"{hashlib.sha256(package.read_bytes()).hexdigest()}  {package.name}\n"
            )
            result = subprocess.run(
                [
                    sys.executable,
                    str(REPO / "tools/build_repository.py"),
                    str(root),
                    str(root / "site"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("wrong launcher architecture", result.stderr)
            self.assertFalse((root / "site").exists())


if __name__ == "__main__":
    unittest.main()
