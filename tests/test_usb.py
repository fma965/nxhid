"""Exercise the USB backend against a simulated libnx service boundary."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="nxhid-usb-tests-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.executable = Path(cls.build.name) / "usb"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "tests/fakes"), "-I" + str(ROOT / "include"),
            str(ROOT / "source/usb_keyboard.c"), str(ROOT / "tests/usb_harness.c"),
            "-o", str(cls.executable),
        ], check=True)

    def test_descriptors_and_partial_reads(self):
        subprocess.run([str(self.executable), "descriptors"], check=True, timeout=5)

    def test_hid_controls_idle_and_disconnect(self):
        subprocess.run([str(self.executable), "controls"], check=True, timeout=5)

    def test_service_errors_short_transfers_and_timeouts(self):
        subprocess.run([str(self.executable), "errors"], check=True, timeout=5)
