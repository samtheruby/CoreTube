# SPDX-License-Identifier: BSD-3-Clause
"""Collect the unmodified ARM runtime files needed by the standalone DRM helper."""

import hashlib
import json
import re
import stat
import zipfile


def validate_bundle(path):
    """Check coretube-widevine.zip; return its manifest. Raise ValueError if unsafe."""
    try:
        archive = zipfile.ZipFile(path)
    except zipfile.BadZipFile as error:
        raise ValueError(f"Not a zip file: {error}") from error
    with archive:
        # Folder entries are harmless: extraction only writes the manifest's files.
        members = [member for member in archive.infolist() if not member.is_dir()]
        if (
            not members
            or len(members) > 65
            or sum(member.file_size for member in members) > 64 * 1024 * 1024
        ):
            raise ValueError("Invalid runtime bundle size")
        files = {}
        for member in members:
            kind = stat.S_IFMT(member.external_attr >> 16)
            if (
                kind not in (0, stat.S_IFREG)
                or member.file_size > 32 * 1024 * 1024
                or member.filename in files
                or not re.fullmatch(
                    # Libraries only: a profile or key file never belongs here.
                    r"runtime-manifest\.json|system/bin/linker|(?:system|vendor)/lib/(?:vndk(?:-sp)?-28/)?[A-Za-z0-9_@.+-]+\.so",
                    member.filename,
                )
            ):
                raise ValueError("Unsafe runtime archive member")
            try:
                files[member.filename] = archive.read(member)
            except zipfile.BadZipFile as error:
                raise ValueError(f"Damaged runtime zip: {error}") from error
        if "runtime-manifest.json" not in files:
            raise ValueError("Missing runtime manifest")
        manifest = json.loads(files.pop("runtime-manifest.json"))
        if (
            not isinstance(manifest, dict)
            or set(manifest) != set(files)
            or not {"system/bin/linker", "vendor/lib/libwvhidl.so"}.issubset(files)
        ):
            raise ValueError("Runtime manifest does not match archive")
        for name, data in files.items():
            expected = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            if manifest[name] != expected:
                raise ValueError("Runtime file integrity check failed")
        return manifest
