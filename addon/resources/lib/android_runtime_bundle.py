# SPDX-License-Identifier: BSD-3-Clause
"""Collect the unmodified ARM runtime files needed by the standalone DRM helper."""

import hashlib
import json
import re
import tarfile


def validate_bundle(path):
    with tarfile.open(path, "r:gz") as archive:
        members = archive.getmembers()
        if (
            not members
            or len(members) > 65
            or sum(member.size for member in members) > 64 * 1024 * 1024
        ):
            raise ValueError("Invalid runtime bundle size")
        files = {}
        for member in members:
            if (
                not member.isfile()
                or member.size < 0
                or member.size > 32 * 1024 * 1024
                or any(part in (".", "..") for part in member.name.split("/"))
                or member.name in files
                or not re.fullmatch(
                    # Libraries only: a profile or key file never belongs here.
                    r"runtime-manifest\.json|system/bin/linker|(?:system|vendor)/lib/(?:vndk(?:-sp)?-28/)?[A-Za-z0-9_@.+-]+\.so",
                    member.name,
                )
            ):
                raise ValueError("Unsafe runtime archive member")
            files[member.name] = archive.extractfile(member).read()
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
