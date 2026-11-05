# SPDX-License-Identifier: BSD-3-Clause
"""Build the Kodi repository from the two checked GitHub release packages."""

import argparse
import hashlib
import re
import shutil
import struct
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent


def build(assets: Path, output: Path) -> None:
    checksums = {}
    for line in (assets / "SHA256SUMS").read_text().splitlines():
        digest, filename = line.split()
        checksums[filename] = digest

    packages = []
    version = ""
    for arch, platform, bits, machine in [
        # ARM CoreELEC's Kodi lacks linux-armv7; repository dirs select its Kodi major.
        ("arm", "linux", 1, 40),
        ("aarch64", "linux-aarch64", 2, 183),
    ]:
        candidates = list(assets.glob(f"browser.tizentube-*-{arch}.zip"))
        if len(candidates) != 1:
            raise ValueError(f"expected one {arch} release package")
        package = candidates[0]
        digest = hashlib.sha256(package.read_bytes()).hexdigest()
        if checksums.get(package.name) != digest:
            raise ValueError(f"checksum mismatch: {package.name}")
        with zipfile.ZipFile(package) as archive:
            addon = ET.fromstring(archive.read("browser.tizentube/addon.xml"))
            current = addon.get("version", "")
            if (
                addon.tag != "addon"
                or addon.get("id") != "browser.tizentube"
                or re.fullmatch(r"\d+\.\d+\.\d+", current) is None
                or package.name != f"browser.tizentube-{current}-{arch}.zip"
                or (version and version != current)
            ):
                raise ValueError(
                    f"invalid or mismatched release metadata: {package.name}"
                )
            version = current
            header = archive.read("browser.tizentube/cobalt/cobalt")[:20]
            if (
                len(header) != 20
                or header[:6] != b"\x7fELF" + bytes([bits, 1])
                or struct.unpack_from("<H", header, 18)[0] != machine
            ):
                raise ValueError(f"wrong launcher architecture: {package.name}")
            metadata = addon.find("extension[@point='xbmc.addon.metadata']")
            if (
                metadata is None
                or metadata.findtext("assets/icon") != "resources/icon.png"
            ):
                raise ValueError(f"missing add-on metadata or icon: {package.name}")
            platform_element = metadata.find("platform")
            if platform_element is None:
                platform_element = ET.SubElement(metadata, "platform")
            platform_element.text = platform
            for path_element in metadata.findall("path"):
                metadata.remove(path_element)
            packages.append(
                (
                    package,
                    arch,
                    digest,
                    archive.read("browser.tizentube/resources/icon.png"),
                    addon,
                )
            )

    repository = ET.parse(SOURCE / "repository/addon.xml").getroot()
    installer_name = f"repository.coretube-{repository.attrib['version']}.zip"
    output.mkdir(parents=True, exist_ok=False)
    installer = output / installer_name
    repository_icon = (SOURCE / "addon/resources/icon.png").read_bytes()
    with zipfile.ZipFile(installer, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(
            "repository.coretube/addon.xml",
            ET.tostring(repository, encoding="utf-8", xml_declaration=True),
        )
        archive.writestr("repository.coretube/resources/icon.png", repository_icon)
    installer_digest = hashlib.sha256(installer.read_bytes()).hexdigest()
    for package, arch, digest, icon, addon in packages:
        feed = output / arch
        destination = feed / "browser.tizentube" / f"browser.tizentube-{version}.zip"
        destination.parent.mkdir(parents=True)
        shutil.copyfile(package, destination)
        Path(str(destination) + ".sha256").write_text(digest + "\n")
        (destination.parent / "resources").mkdir()
        (destination.parent / "resources/icon.png").write_bytes(icon)

        repository_dir = feed / "repository.coretube"
        (repository_dir / "resources").mkdir(parents=True)
        (repository_dir / "resources/icon.png").write_bytes(repository_icon)
        shutil.copyfile(installer, repository_dir / installer_name)
        (repository_dir / (installer_name + ".sha256")).write_text(
            installer_digest + "\n"
        )
        index = ET.Element("addons")
        index.extend((addon, repository))
        data = ET.tostring(index, encoding="utf-8", xml_declaration=True)
        (feed / "addons.xml").write_bytes(data)
        (feed / "addons.xml.sha256").write_text(hashlib.sha256(data).hexdigest() + "\n")
    page = (SOURCE / "repository/index.html").read_text()
    (output / "index.html").write_text(
        page.replace("{{repository_zip}}", installer_name).replace(
            "{{version}}", version
        )
    )
    (output / ".nojekyll").touch()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "assets", type=Path, help="downloaded release ZIPs and SHA256SUMS"
    )
    parser.add_argument("output", type=Path, help="new directory for the Pages site")
    args = parser.parse_args()
    try:
        build(args.assets, args.output)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, ET.ParseError) as error:
        parser.exit(1, f"build_repository: {error}\n")
