"""Offline tests for renner-gpu-detect, using a fake sysfs tree.

Run: python -m unittest discover -s tests
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_loader = importlib.machinery.SourceFileLoader(
    "renner_gpu_detect", str(ROOT / "system/usr/bin/renner-gpu-detect")
)
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
detect = importlib.util.module_from_spec(_spec)
sys.modules[_loader.name] = detect  # dataclasses look the module up here
_loader.exec_module(detect)

# Real PCI device IDs.
NV = 0x10DE
RTX_4090 = 0x2684      # Ada
GTX_1650 = 0x1F82      # Turing TU117, first range that runs nvidia-open
TITAN_V = 0x1D81       # Volta, last legacy generation
GTX_1080 = 0x1B80      # Pascal
GTX_750_TI = 0x1380    # Maxwell GM107
GTX_680 = 0x1180       # Kepler
RX_7900, UHD_770, VIRTIO_GPU = 0x744C, 0xA780, 0x1050


class GpuDetectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sysfs = Path(self.tmp.name)
        self.count = 0

    def tearDown(self):
        self.tmp.cleanup()

    def add_device(self, vendor, device, pci_class=0x030000, boot_vga=None):
        # Real sysfs names are 0000:01:00.0; Windows forbids ':' in names and
        # the detector never parses them, so '_' keeps the test portable.
        dev = self.sysfs / f"0000_{self.count:02x}_00.0"
        self.count += 1
        dev.mkdir()
        (dev / "class").write_text(f"0x{pci_class:06x}\n")
        (dev / "vendor").write_text(f"0x{vendor:04x}\n")
        (dev / "device").write_text(f"0x{device:04x}\n")
        if boot_vga is not None:
            (dev / "boot_vga").write_text(f"{boot_vga}\n")

    def run_detect(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = detect.main(["--sysfs", str(self.sysfs)])
        profile = {}
        for line in out.getvalue().splitlines():
            key, value = line.split("=", 1)
            if not key.removeprefix("RENNER_GPU_").isdigit():  # skip per-device lines
                profile[key] = value.strip('"')
        profile["packages"] = profile["RENNER_GPU_PACKAGES"].split()
        return code, profile

    def test_amd_only(self):
        self.add_device(0x1002, RX_7900, boot_vga=1)
        self.add_device(0x1002, 0x1234, pci_class=0x040300)  # HDMI audio, not a GPU
        code, p = self.run_detect()
        self.assertEqual(code, 0)
        self.assertEqual(p["RENNER_GPU_COUNT"], "1")
        self.assertEqual(p["RENNER_GPU_NVIDIA_DRIVER"], "none")
        self.assertIn("vulkan-radeon", p["packages"])
        self.assertIn("lib32-mesa", p["packages"])
        self.assertNotIn("nvidia-open", p["packages"])

    def test_turing_and_newer_use_open_modules(self):
        for device in (RTX_4090, GTX_1650):
            with self.subTest(device=hex(device)):
                self.setUp()
                self.add_device(NV, device, boot_vga=1)
                code, p = self.run_detect()
                self.assertEqual((code, p["RENNER_GPU_NVIDIA_DRIVER"]), (0, "open"))
                self.assertIn("nvidia-open", p["packages"])
                self.assertIn("lib32-nvidia-utils", p["packages"])

    def test_maxwell_pascal_volta_need_legacy_branch(self):
        for device in (TITAN_V, GTX_1080, GTX_750_TI):
            with self.subTest(device=hex(device)):
                self.setUp()
                self.add_device(NV, device, boot_vga=1)
                code, p = self.run_detect()
                self.assertEqual((code, p["RENNER_GPU_NVIDIA_DRIVER"]), (4, "legacy-580"))
                self.assertNotIn("nvidia-open", p["packages"])
                self.assertIn("vulkan-nouveau", p["packages"])

    def test_one_legacy_card_moves_whole_machine_to_legacy(self):
        self.add_device(NV, RTX_4090, boot_vga=1)
        self.add_device(NV, GTX_1080)
        _, p = self.run_detect()
        self.assertEqual(p["RENNER_GPU_NVIDIA_DRIVER"], "legacy-580")
        self.assertNotIn("nvidia-open", p["packages"])

    def test_kepler_uses_nouveau(self):
        self.add_device(NV, GTX_680, boot_vga=1)
        code, p = self.run_detect()
        self.assertEqual((code, p["RENNER_GPU_NVIDIA_DRIVER"]), (0, "nouveau"))
        self.assertIn("vulkan-nouveau", p["packages"])

    def test_hybrid_laptop(self):
        self.add_device(0x8086, UHD_770, boot_vga=1)
        self.add_device(NV, RTX_4090, pci_class=0x030200)  # 3D controller
        _, p = self.run_detect()
        self.assertEqual(p["RENNER_GPU_VENDORS"], "intel,nvidia")
        self.assertEqual(p["RENNER_GPU_MULTI"], "1")
        for pkg in ("vulkan-intel", "nvidia-open", "switcheroo-control", "nvidia-prime"):
            self.assertIn(pkg, p["packages"])

    def test_virtual_gpu_gets_generic_mesa(self):
        self.add_device(0x1AF4, VIRTIO_GPU, boot_vga=1)
        code, p = self.run_detect()
        self.assertEqual((code, p["RENNER_GPU_VENDORS"]), (0, "other"))
        self.assertIn("mesa", p["packages"])
        self.assertNotIn("vulkan-radeon", p["packages"])


if __name__ == "__main__":
    unittest.main()
