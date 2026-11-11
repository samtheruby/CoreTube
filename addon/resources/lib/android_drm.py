# SPDX-License-Identifier: BSD-3-Clause
"""Run Cobalt and the Android L3 helper with private inherited descriptors."""

import contextlib
import fcntl
import os
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import zipfile
from http.client import HTTPException
from pathlib import Path

from android_provision import request_provision
from android_runtime_bundle import validate_bundle

MAX_PACKET = 1024 * 1024
CAPACITY = 16 * 1024 * 1024
# The Widevine libraries do not ship with the add-on: the user copies this zip
# to the add-on's widevine folder or to one of the shared places.
RUNTIME_NAME = "coretube-widevine.zip"
SHARED_PLACES = ("/storage/.config", "/flash", "/storage")


def find_runtime(home, shared=SHARED_PLACES):
    """The Widevine zip in HOME/widevine, else in the first shared place that has it."""
    own = home / "widevine"
    own.mkdir(parents=True, exist_ok=True)
    for folder in (own, *map(Path, shared)):
        if (folder / RUNTIME_NAME).is_file():
            return folder / RUNTIME_NAME
    return None


def receive_exact(channel, size):
    result = bytearray()
    while len(result) < size:
        data = channel.recv(size - len(result))
        if not data:
            raise EOFError("Android DRM channel closed")
        result.extend(data)
    return bytes(result)


def receive_packet(channel):
    size = struct.unpack(">I", receive_exact(channel, 4))[0]
    if not 4 <= size <= MAX_PACKET:
        raise ValueError("Invalid Android DRM packet length")
    return receive_exact(channel, size)


def send_packet(channel, data):
    if not 4 <= len(data) <= MAX_PACKET:
        raise ValueError("Invalid Android DRM packet length")
    channel.sendall(struct.pack(">I", len(data)) + data)


def provision(channel):
    """Only provisioning goes here; content licenses stay with Cobalt's app."""
    packet = receive_packet(channel)
    position = 0

    def integer():
        nonlocal position
        if len(packet) - position < 4:
            raise ValueError("Truncated provisioning packet")
        value = struct.unpack_from(">I", packet, position)[0]
        position += 4
        return value

    def field():
        nonlocal position
        size = integer()
        if size > len(packet) - position:
            raise ValueError("Truncated provisioning field")
        value = packet[position : position + size]
        position += size
        return value

    try:
        if integer() != 7:
            raise ValueError("Unexpected provisioning command")
        url, request = field().decode("utf-8"), field()
        if position != len(packet) or not request:
            raise ValueError("Invalid provisioning request")
        response = request_provision(url, request)
        send_packet(channel, struct.pack(">II", 0, len(response)) + response)
        print("android-drm: provisioning completed", flush=True)
    except (OSError, ValueError, RuntimeError, HTTPException) as error:
        print(f"android-drm: provisioning failed: {error}", flush=True)
        send_packet(channel, struct.pack(">I", 1))


@contextlib.contextmanager
def runtime(addon, home, archive, manifest):
    profile = home / "android-drm"
    profile.mkdir(mode=0o700, parents=True, exist_ok=True)
    if profile.is_symlink():
        raise ValueError("DRM profile must be a private directory")
    os.chmod(profile, 0o700)
    with (profile / "lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        root = Path(tempfile.mkdtemp(prefix="tizentube-drm.", dir="/run"))
        mounts = []
        try:
            # Validation completes before any member is written.
            with zipfile.ZipFile(archive) as source:
                for name in manifest:
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(name) as data, target.open("xb") as output:
                        shutil.copyfileobj(data, output)
                    target.chmod(0o755 if name == "system/bin/linker" else 0o644)
            for name in ("proc", "dev", "tmp", "data"):
                (root / name).mkdir(mode=0o700)
            for name, minor in (
                ("null", 3),
                ("zero", 5),
                ("random", 8),
                ("urandom", 9),
            ):
                os.mknod(
                    root / "dev" / name, stat.S_IFCHR | 0o600, os.makedev(1, minor)
                )
            shutil.copyfile(
                addon / "drm" / "android-drm-helper", root / "android-drm-helper"
            )
            (root / "android-drm-helper").chmod(0o700)
            for relative in ("vendor/mediadrm", "mediadrm"):
                source = profile / relative
                source.mkdir(mode=0o700, parents=True, exist_ok=True)
                if source.is_symlink():
                    raise ValueError("DRM state must be a private directory")
                target = root / "data" / relative
                target.mkdir(mode=0o700, parents=True)
                subprocess.run(
                    ["mount", "--bind", str(source), str(target)], check=True
                )
                mounts.append(target)
            subprocess.run(
                [
                    "mount",
                    "-t",
                    "proc",
                    "-o",
                    "ro,nosuid,nodev,noexec",
                    "proc",
                    str(root / "proc"),
                ],
                check=True,
            )
            mounts.append(root / "proc")
            yield root
        finally:
            failures = []
            for target in reversed(mounts):
                result = subprocess.run(["umount", str(target)], check=False)
                if result.returncode:
                    failures.append(str(target))
            if failures:
                raise RuntimeError("DRM cleanup failed: " + ", ".join(failures))
            shutil.rmtree(root)


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def run(addon, home, command):
    os.umask(0o077)
    archive = find_runtime(Path(home))
    manifest = None
    if archive is None:
        print(
            f"android-drm: no {RUNTIME_NAME} in {Path(home) / 'widevine'} or "
            f"{', '.join(SHARED_PLACES)}; protected videos will not play",
            flush=True,
        )
    else:
        try:
            manifest = validate_bundle(archive)
            print(f"android-drm: Widevine from {archive}", flush=True)
        except (OSError, ValueError) as error:
            print(
                f"android-drm: {archive} is not usable ({error}); "
                "protected videos will not play",
                flush=True,
            )
    if manifest is None:
        # Cobalt plays clear videos without the helper's descriptors.
        return os.execv(command[0], command)
    stopping = False

    def shutdown(_signum, _frame):
        nonlocal stopping
        stopping = True

    previous = {
        sig: signal.signal(sig, shutdown) for sig in (signal.SIGTERM, signal.SIGINT)
    }
    try:
        with runtime(Path(addon), Path(home), archive, manifest) as root:
            client, helper_channel = socket.socketpair()
            broker, helper_broker = socket.socketpair()
            with (
                client,
                helper_channel,
                broker,
                helper_broker,
                tempfile.TemporaryFile(dir=root / "tmp") as shared,
            ):
                shared.truncate(2 * CAPACITY)
                broker.settimeout(20)
                fd, memory_fd = helper_channel.fileno(), shared.fileno()
                environment = {
                    "LD_LIBRARY_PATH": "/vendor/lib:/system/lib/vndk-28:/system/lib/vndk-sp-28:/system/lib",
                    "ANDROID_DATA": "/data",
                    "ANDROID_ROOT": "/system",
                    "HOME": "/tmp",
                }
                helper = cobalt = None
                try:
                    helper = subprocess.Popen(
                        [
                            "/usr/sbin/chroot",
                            str(root),
                            "/android-drm-helper",
                            str(fd),
                            str(memory_fd),
                            str(helper_broker.fileno()),
                        ],
                        env=environment,
                        pass_fds=(fd, memory_fd, helper_broker.fileno()),
                    )
                    helper_channel.close()
                    helper_broker.close()
                    environment = dict(
                        os.environ,
                        TIZENTUBE_DRM_FD=str(client.fileno()),
                        TIZENTUBE_DRM_BUFFER_FD=str(memory_fd),
                    )
                    cobalt = subprocess.Popen(
                        command, env=environment, pass_fds=(client.fileno(), memory_fd)
                    )
                    client.close()
                    while (
                        not stopping and cobalt.poll() is None and helper.poll() is None
                    ):
                        if select.select([broker], [], [], 0.1)[0]:
                            try:
                                provision(broker)
                            except EOFError:
                                if stopping:
                                    return 0
                                try:
                                    return cobalt.wait(timeout=1)
                                except subprocess.TimeoutExpired:
                                    return 1
                    if stopping:
                        return 0
                    return cobalt.returncode if cobalt.poll() is not None else 1
                finally:
                    stop(cobalt)
                    stop(helper)
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
