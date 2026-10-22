import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package", ROOT / "tools/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackagingTests(unittest.TestCase):
    def test_package_excludes_stale_files_and_metadata(self):
        with tempfile.TemporaryDirectory(prefix="nxhid-package-tests-") as directory:
            root = Path(directory)
            for source in package.FILES:
                path = root / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(source.encode())
            stale = root / "dist/switch/.DS_Store"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"metadata")
            (root / "dist/old-release.zip").write_bytes(b"stale")
            archive = package.build_package(root, "0.1.0")
            with ZipFile(archive) as bundle:
                self.assertIsNone(bundle.testzip())
                self.assertEqual(set(bundle.namelist()), set(package.FILES.values()))
                for source, destination in package.FILES.items():
                    self.assertEqual(bundle.read(destination), source.encode())
                    self.assertEqual((root / "dist" / destination).read_bytes(), source.encode())
            self.assertFalse(stale.exists())
            self.assertFalse((root / "dist/old-release.zip").exists())

    def test_host_targets_do_not_require_switch_sdk(self):
        result = subprocess.run([
            "make", "--no-print-directory", "-n", "test", "clean",
            "DEVKITPRO=/nonexistent-nxhid-sdk",
        ], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
