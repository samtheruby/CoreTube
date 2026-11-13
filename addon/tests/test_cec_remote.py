# SPDX-License-Identifier: BSD-3-Clause
import importlib.util
import os
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "resources" / "lib")
)
import cec_remote
from cec_remote import KEY_ENTER, KEY_ESC, KEY_PLAYPAUSE, KEY_UP, Keys


def press(code):
    return (0x44, [code])


RELEASE = (0x45, [])


class TrafficTests(unittest.TestCase):
    def test_received_frames_only(self):
        self.assertEqual(
            cec_remote.received_frame("TRAFFIC: [          123456]\t>> 04:44:01"),
            (0x44, [0x01]),
        )
        self.assertEqual(
            cec_remote.received_frame("TRAFFIC: [             9]\t>> 04:45"),
            (0x45, []),
        )
        for line in (
            "TRAFFIC: [   1]\t<< 40:44:01",  # one we sent
            "TRAFFIC: [   1]\t>> 0f",  # a poll: no opcode
            "DEBUG:   [   1]\t>> 04:44:01",
            "TRAFFIC: [   1]\t>> 04:zz",
            "waiting for input",
        ):
            with self.subTest(line=line):
                self.assertIsNone(cec_remote.received_frame(line))


class KeyTests(unittest.TestCase):
    def test_press_repeat_release(self):
        keys = Keys()
        self.assertEqual(keys.frame(press(0x01), 0.0), [(KEY_UP, 1)])
        # The TV repeats the press while the button is held.
        self.assertEqual(keys.frame(press(0x01), 0.4), [(KEY_UP, 2)])
        self.assertEqual(keys.frame(RELEASE, 0.5), [(KEY_UP, 0)])
        self.assertEqual(keys.frame(RELEASE, 0.6), [])

    def test_another_button_releases_the_first(self):
        keys = Keys()
        keys.frame(press(0x00), 0.0)
        self.assertEqual(keys.frame(press(0x0D), 0.1), [(KEY_ENTER, 0), (KEY_ESC, 1)])

    def test_release_without_a_release_message(self):
        # CEC: a held button repeats within 550 ms; silence means released.
        keys = Keys()
        keys.frame(press(0x61), 1.0)
        self.assertEqual(keys.timeout(1.2), 0.35)
        self.assertEqual(keys.tick(1.5), [])
        self.assertEqual(keys.tick(1.55), [(KEY_PLAYPAUSE, 0)])
        self.assertIsNone(keys.timeout(2.0))

    def test_other_messages_and_buttons_do_nothing(self):
        keys = Keys()
        self.assertEqual(keys.frame(press(0x7F), 0.0), [])  # not mapped
        self.assertEqual(keys.frame((0x44, []), 0.0), [])  # no button code
        self.assertEqual(keys.frame((0x8F, []), 0.0), [])  # give power status
        self.assertIsNone(keys.timeout(0.0))

    def test_back_and_media_buttons(self):
        for code, key in (
            (0x0D, KEY_ESC),
            (0x91, KEY_ESC),
            (0x44, KEY_PLAYPAUSE),
            (0x46, KEY_PLAYPAUSE),
            (0x61, KEY_PLAYPAUSE),
        ):
            with self.subTest(code=code):
                self.assertEqual(Keys().frame(press(code), 0.0), [(key, 1)])


def load_runner():
    path = pathlib.Path(__file__).resolve().parents[1] / "bin" / "cec-remote.py"
    spec = importlib.util.spec_from_file_location("cec_remote_runner", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunnerTests(unittest.TestCase):
    def test_cec_client_frames_become_key_events(self):
        runner = load_runner()
        with tempfile.TemporaryDirectory() as folder:
            client = pathlib.Path(folder) / "cec-client"
            client.write_text(
                "#!/bin/sh\n"
                "echo 'TRAFFIC: [               1]\t<< 40:8f'\n"
                "printf 'TRAFFIC: [               2]\\t>> 04:44:00\\n'\n"
                "printf 'TRAFFIC: [               3]\\t>> 04:44:00\\n'\n"
                "printf 'TRAFFIC: [               4]\\t>> 04:45\\n'\n"
            )
            client.chmod(0o755)
            read, write = os.pipe()
            with mock.patch.dict(os.environ, {"PATH": folder}):
                runner.listen(write, "CoreELEC")
            os.close(write)
            data = os.read(read, 4096)
            os.close(read)
        size = struct.calcsize("@llHHi")
        events = [
            struct.unpack("@llHHi", data[i : i + size])[2:]
            for i in range(0, len(data), size)
        ]
        self.assertEqual(
            events,
            [
                (1, KEY_ENTER, 1), (0, 0, 0),
                (1, KEY_ENTER, 2), (0, 0, 0),
                (1, KEY_ENTER, 0), (0, 0, 0),
            ],
        )  # fmt: skip

    def test_off_without_cec(self):
        runner = load_runner()
        with tempfile.TemporaryDirectory() as folder:
            settings = pathlib.Path(folder) / "cec_CEC_Adapter.xml"
            settings.write_text(
                '<settings><setting id="enabled" value="0"/></settings>'
            )
            with mock.patch.object(runner, "create_keyboard") as create:
                self.assertEqual(runner.main(settings), 0)
                with mock.patch.dict(os.environ, {"PATH": folder}):
                    self.assertEqual(runner.main(pathlib.Path(folder) / "none"), 0)
            create.assert_not_called()


class SettingsTests(unittest.TestCase):
    def test_kodi_cec_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "cec_CEC_Adapter.xml"
            self.assertEqual(cec_remote.kodi_settings(path), (True, "CoreELEC"))
            path.write_text(
                '<settings>\n    <setting id="device_name" value="Living room"/>\n'
                '    <setting id="enabled" value="0"/>\n</settings>\n'
            )
            self.assertEqual(cec_remote.kodi_settings(path), (False, "Living room"))
            path.write_text("not xml")
            self.assertEqual(cec_remote.kodi_settings(path), (True, "CoreELEC"))


if __name__ == "__main__":
    unittest.main()
