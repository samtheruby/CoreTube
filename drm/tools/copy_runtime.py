# SPDX-License-Identifier: BSD-3-Clause
"""Copy a Fire TV Cube's Widevine runtime once, using read-only Android mounts.

Usage: copy_runtime.py --host HOST --helper HELPER [--output ARCHIVE]
Connects as root over SSH (password in BOX_PW), mounts Android system and vendor
read-only without journal replay, copies the unmodified libraries the helper and
libwvhidl.so need plus the Android linker, then unmounts. Nothing on the box is
written outside a temporary folder in /run, and no DRM state is read.
"""

import argparse
import io
import os
import pathlib
import re
import shlex

import paramiko
from runtime_bundle import collect_runtime, write_bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", required=True, help="the Cube running CoreELEC")
    parser.add_argument(
        "--helper", required=True, type=pathlib.Path, help="built android-drm-helper"
    )
    parser.add_argument(
        "--output", type=pathlib.Path, default=pathlib.Path("coretube-widevine.zip")
    )
    args = parser.parse_args()
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    root = None
    mounts = []

    def run(command):
        _, output, errors = client.exec_command(command, timeout=30)
        text = output.read().decode(errors="replace")
        error = errors.read().decode(errors="replace")
        if output.channel.recv_exit_status():
            raise RuntimeError(error or text or "Remote command failed")
        return text.strip()

    try:
        client.connect(
            args.host,
            username="root",
            password=os.environ["BOX_PW"],
            look_for_keys=False,
            allow_agent=False,
            timeout=10,
        )
        candidate = run("mktemp -d /run/tizentube-runtime-source.XXXXXX")
        if not re.fullmatch(
            r"/run/tizentube-runtime-source\.[A-Za-z0-9]{6}", candidate
        ):
            raise RuntimeError("Unexpected temporary directory")
        root = candidate
        for partition in ("system", "vendor"):
            target = root + "/" + partition
            run("mkdir " + shlex.quote(target))
            run(f"mount -t ext4 -o ro,noload /dev/{partition} {shlex.quote(target)}")
            mounts.append(target)
        table = run("awk '$2 ~ /tizentube-runtime-source/ {print $2,$4}' /proc/mounts")
        options = {
            path: set(flags.split(","))
            for path, flags in (line.split() for line in table.splitlines())
        }
        if any(
            not {"ro", "norecovery"}.issubset(options.get(mount, set()))
            for mount in mounts
        ):
            raise RuntimeError(
                "Android source mount must disable writes and journal replay"
            )
        print(table, flush=True)
        with client.open_sftp() as sftp:

            def source_path(path):
                if path.startswith("/system/"):
                    return root + "/system/system/" + path[len("/system/") :]
                if path.startswith("/vendor/"):
                    return root + path
                raise ValueError("Runtime files must come from system or vendor")

            def exists(path):
                try:
                    sftp.stat(source_path(path))
                    return True
                except FileNotFoundError:
                    return False

            def read_file(path):
                remote = source_path(path)
                if sftp.stat(remote).st_size > 32 * 1024 * 1024:
                    raise ValueError("Runtime file exceeds size limit")
                data = io.BytesIO()
                sftp.getfo(
                    remote, data, prefetch=True, max_concurrent_prefetch_requests=16
                )
                print("runtime:copy " + path, flush=True)
                return data.getvalue()

            files = collect_runtime(read_file, exists, args.helper.read_bytes())
        target = args.output
        manifest = write_bundle(target, files)
        print(
            f"runtime:bundle files={len(manifest)} bytes={sum(len(data) for data in files.values())}"
        )
        print(target)
    finally:
        if client.get_transport() and client.get_transport().is_active():
            for mount in reversed(mounts):
                run("umount " + shlex.quote(mount))
            if root:
                for partition in ("system", "vendor"):
                    run("rmdir " + shlex.quote(root + "/" + partition))
                run("rmdir " + shlex.quote(root))
                print("runtime:source-unmounted", flush=True)
        client.close()


if __name__ == "__main__":
    main()
