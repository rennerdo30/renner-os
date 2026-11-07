"""Offline tests for renner-gpu-detect, using a fake sysfs tree.

Run: python -m unittest discover -s tests
"""

import contextlib
import importlib.machinery
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_loader = importlib.machinery.SourceFileLoader(
    "renner_gpu_detect", str(ROOT / "packages/renner-gpu/renner-gpu-detect")
)
_spec = importlib.util.spec_from_loader(_loader.name, _loader)
detect = importlib.util.module_from_spec(_spec)
sys.modules[_loader.name] = detect  # dataclasses look the module up here
_loader.exec_module(detect)

# Real device ids: RTX 4090 (Ada), GTX 1080 (Pascal), GTX 680 (Kepler),
# Radeon RX 7900 XTX, Intel Raptor Lake UHD 770.
RTX_4090, GTX_1080, GTX_680 = 0x2684, 0x1B80, 0x1180
RX_7900, UHD_770 = 0x744C, 0xA780


class GpuDetectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.sysfs = self.root / "devices"
        self.sysfs.mkdir()
        self.open_list = self.root / "open.json"
        self.legacy_list = self.root / "580.json"
        self._write_list(self.open_list, {RTX_4090: ["kernelopen"], GTX_1080: []})
        self._write_list(self.legacy_list, {RTX_4090: ["kernelopen"], GTX_1080: []})
        self.count = 0

    def tearDown(self):
        self.tmp.cleanup()

    def _write_list(self, path, chips):
        path.write_text(
            json.dumps({"chips": [{"devid": f"0x{d:04X}", "features": f} for d, f in chips.items()]})
        )

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

    def run_detect(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = detect.main(
                [
                    "--sysfs", str(self.sysfs),
                    "--nvidia-open-list", str(self.open_list),
                    "--nvidia-legacy-list", str(self.legacy_list),
                    *extra,
                ]
            )
        profile = {}
        for line in out.getvalue().splitlines():
            key, value = line.split("=", 1)
            if not key.removeprefix("RENNER_GPU_").isdigit():  # skip per-device lines
                profile[key] = value
        return code, profile

    def test_amd_only_is_standard_mesa(self):
        self.add_device(0x1002, RX_7900, boot_vga=1)
        self.add_device(0x1002, 0x1234, pci_class=0x040300)  # HDMI audio, not a GPU
        code, p = self.run_detect()
        self.assertEqual(code, 0)
        self.assertEqual(p["RENNER_GPU_COUNT"], "1")
        self.assertEqual(p["RENNER_GPU_VARIANT"], "standard")
        self.assertEqual(p["RENNER_GPU_NVIDIA_DRIVER"], "none")
        self.assertEqual(p["RENNER_GPU_MULTI"], "0")

    def test_turing_plus_uses_open_modules(self):
        self.add_device(0x10DE, RTX_4090, boot_vga=1)
        code, p = self.run_detect()
        self.assertEqual((code, p["RENNER_GPU_NVIDIA_DRIVER"], p["RENNER_GPU_VARIANT"]), (0, "open", "standard"))

    def test_pascal_needs_legacy_variant(self):
        self.add_device(0x10DE, GTX_1080, boot_vga=1)
        _, p = self.run_detect()
        self.assertEqual(p["RENNER_GPU_NVIDIA_DRIVER"], "legacy-580")
        self.assertEqual(p["RENNER_GPU_VARIANT"], "nvidia-legacy")

    def test_one_legacy_card_moves_whole_machine_to_legacy(self):
        self.add_device(0x10DE, RTX_4090, boot_vga=1)
        self.add_device(0x10DE, GTX_1080)
        _, p = self.run_detect()
        self.assertEqual(p["RENNER_GPU_VARIANT"], "nvidia-legacy")

    def test_kepler_falls_back_to_nouveau(self):
        self.add_device(0x10DE, GTX_680, boot_vga=1)
        _, p = self.run_detect()
        self.assertEqual((p["RENNER_GPU_NVIDIA_DRIVER"], p["RENNER_GPU_VARIANT"]), ("nouveau", "standard"))

    def test_hybrid_laptop_is_multi_gpu(self):
        self.add_device(0x8086, UHD_770, boot_vga=1)
        self.add_device(0x10DE, RTX_4090, pci_class=0x030200)  # 3D controller
        _, p = self.run_detect()
        self.assertEqual(p["RENNER_GPU_VENDORS"], "intel,nvidia")
        self.assertEqual(p["RENNER_GPU_MULTI"], "1")
        self.assertEqual(p["RENNER_GPU_NVIDIA_DRIVER"], "open")

    def test_nvidia_without_support_lists_fails_loudly(self):
        self.add_device(0x10DE, RTX_4090)
        self.open_list.unlink()
        self.legacy_list.unlink()
        code, _ = self.run_detect()
        self.assertEqual(code, 3)

    def test_variant_mismatch_is_reported(self):
        self.add_device(0x10DE, GTX_1080)
        variant = self.root / "variant"
        variant.write_text("standard\n")
        code, _ = self.run_detect("--check-variant", str(variant))
        self.assertEqual(code, 1)
        variant.write_text("nvidia-legacy\n")
        code, _ = self.run_detect("--check-variant", str(variant))
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
