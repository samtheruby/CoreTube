import contextlib
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

LIB = pathlib.Path(__file__).resolve().parents[1] / "resources" / "lib"
sys.path.insert(0, str(LIB))


class AndroidRuntimeTests(unittest.TestCase):
    def test_broker_close_returns_cobalt_status(self):
        import android_drm

        for child_status, expected in (
            (0, 0),
            (17, 17),
            (subprocess.TimeoutExpired("cobalt", 1), 1),
        ):
            with (
                self.subTest(status=child_status),
                tempfile.TemporaryDirectory() as folder,
            ):
                root = pathlib.Path(folder)
                (root / "tmp").mkdir()
                helper, cobalt = Mock(), Mock()
                helper.poll.return_value = cobalt.poll.return_value = None
                if isinstance(child_status, Exception):
                    cobalt.wait.side_effect = [child_status, 0]
                else:
                    cobalt.wait.return_value = child_status
                with (
                    patch.object(
                        android_drm,
                        "runtime",
                        return_value=contextlib.nullcontext(root),
                    ),
                    patch.object(
                        android_drm.subprocess, "Popen", side_effect=[helper, cobalt]
                    ),
                    patch.object(android_drm.os, "umask"),
                ):
                    self.assertEqual(
                        android_drm.run("unused", folder, ["unused"]), expected
                    )

    def test_binary_packets_and_truncation(self):
        import android_drm

        left, right = socket.socketpair()
        with left, right:
            android_drm.send_packet(left, b"hello\0world")
            self.assertEqual(android_drm.receive_packet(right), b"hello\0world")
            left.sendall(struct.pack(">I", android_drm.MAX_PACKET + 1))
            with self.assertRaises(ValueError):
                android_drm.receive_packet(right)

    def test_provisioning_rejects_other_hosts(self):
        from android_provision import request_provision

        for url in (
            "https://example.com/",
            "http://www.googleapis.com/",
            "https://www.googleapis.com@localhost/",
        ):
            with self.assertRaises(ValueError):
                request_provision(url, b"opaque")


if __name__ == "__main__":
    unittest.main()
