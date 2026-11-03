# SPDX-License-Identifier: BSD-3-Clause
"""Collect, write and check the Cube's Android Widevine runtime bundle.

Usage: runtime_bundle.py check ARCHIVE
Exit 0 when every file matches the manifest and is a library or the linker;
exit 1 otherwise (for example a provisioned profile or key file).
"""

import collections
import hashlib
import io
import json
import pathlib
import re
import sys
import tarfile

# One check for the build and the add-on: the add-on runs the same one on the box.
sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[2] / "addon" / "resources" / "lib")
)
from android_runtime_bundle import validate_bundle


def elf_dependencies(data):
    from elftools.elf.elffile import ELFFile

    elf = ELFFile(io.BytesIO(data))
    if elf.elfclass != 32 or elf.header["e_machine"] != "EM_ARM":
        raise ValueError("Runtime requires 32-bit ARM ELF files")
    dynamic = elf.get_section_by_name(".dynamic")
    return (
        [tag.needed for tag in dynamic.iter_tags() if tag.entry.d_tag == "DT_NEEDED"]
        if dynamic
        else []
    )


def collect_runtime(read_file, exists, helper):
    """The linker, libwvhidl.so and every library they and the helper ELF need."""
    search = (
        "/vendor/lib/",
        "/system/lib/vndk-28/",
        "/system/lib/vndk-sp-28/",
        "/system/lib/",
    )

    def resolve(name):
        if not re.fullmatch(r"[A-Za-z0-9_@.+-]+", name) or name in (".", ".."):
            raise ValueError("Unsafe ELF dependency name")
        for prefix in search:
            if exists(prefix + name):
                return prefix + name
        raise ValueError(f"Missing runtime dependency: {name}")

    pending = collections.deque(["/system/bin/linker", "/vendor/lib/libwvhidl.so"])
    pending.extend(resolve(name) for name in elf_dependencies(helper))
    files = {}
    total = 0
    while pending:
        path = pending.popleft()
        if path in files:
            continue
        if not exists(path):
            raise ValueError(f"Missing runtime file: {path}")
        data = read_file(path)
        total += len(data)
        if (
            not data
            or len(data) > 32 * 1024 * 1024
            or total > 64 * 1024 * 1024
            or len(files) >= 64
        ):
            raise ValueError("Runtime file or bundle exceeds size limit")
        files[path] = data
        pending.extend(resolve(name) for name in elf_dependencies(data))
    return files


def write_bundle(path, files):
    manifest = {
        name.lstrip("/"): {
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        for name, data in sorted(files.items())
    }
    with tarfile.open(path, "w:gz") as archive:
        for name, data in sorted(files.items()):
            entry = tarfile.TarInfo(name.lstrip("/"))
            entry.size = len(data)
            entry.mode = 0o755 if name == "/system/bin/linker" else 0o644
            archive.addfile(entry, io.BytesIO(data))
        data = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
        entry = tarfile.TarInfo("runtime-manifest.json")
        entry.mode, entry.size = 0o644, len(data)
        archive.addfile(entry, io.BytesIO(data))
    return manifest


def main(argv):
    if len(argv) != 2 or argv[0] != "check":
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        manifest = validate_bundle(argv[1])
    except (OSError, ValueError, tarfile.TarError) as error:
        print(f"runtime check failed: {error}", file=sys.stderr)
        return 1
    print(f"runtime ok: {len(manifest)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
