"""Checks release identity and tamper detection using only generated, nonsecret fixtures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("package_release", Path(__file__).parents[1] / ".github/scripts/package_release.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
COMMIT = "a" * 40


class ReleasePackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        for name in module.REQUIRED:
            path = self.bundle / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        (self.bundle / "quant_price_live.exe").write_bytes(b"MZ-fixture-only")
        (self.bundle / "VERSION.json").write_text(json.dumps({"version": "6.1.0", "commit": COMMIT, "platform": "Windows x64"}))
        self.hashes()

    def hashes(self):
        lines = [hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(self.bundle).as_posix()
                 for p in sorted(self.bundle.rglob("*")) if p.is_file() and p.name != "SHA256SUMS.txt"]
        (self.bundle / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8-sig")

    def test_complete_release_roundtrip(self):
        result = module.prepare_release(self.bundle, self.root / "out", COMMIT, "6.1.0")
        module.verify_archive(result["asset"], COMMIT, "6.1.0")
        self.assertEqual(hashlib.sha256(Path(result["asset"]).read_bytes()).hexdigest(), result["sha256"])
        self.assertIn(result["sha256"], Path(result["checksum"]).read_text())

    def test_changed_payload_is_rejected(self):
        (self.bundle / "WORKBENCH_GUIDE.html").write_bytes(b"changed")
        with self.assertRaises(ValueError): module.prepare_release(self.bundle, self.root / "out", COMMIT, "6.1.0")

    def test_wrong_commit_or_version_is_rejected(self):
        with self.assertRaises(ValueError): module.verify_bundle(self.bundle, "b" * 40, "6.1.0")
        with self.assertRaises(ValueError): module.verify_bundle(self.bundle, COMMIT, "6.2.0")

    def test_omitted_and_extra_files_are_rejected(self):
        (self.bundle / "unhashed.dll").write_bytes(b"unexpected")
        with self.assertRaises(ValueError): module.verify_bundle(self.bundle, COMMIT, "6.1.0")
        (self.bundle / "unhashed.dll").unlink()
        (self.bundle / "Qt6Core.dll").unlink()
        self.hashes()
        with self.assertRaises(ValueError): module.verify_bundle(self.bundle, COMMIT, "6.1.0")

    def test_duplicate_and_unsafe_checksums_are_rejected(self):
        for name in ("../secret", "/absolute", "C:/alternate", "a/../b"):
            with self.assertRaises(ValueError): module.checksums("0" * 64 + "  " + name)
        line = "0" * 64 + "  VERSION.json\n"
        with self.assertRaises(ValueError): module.checksums(line + line)


if __name__ == "__main__":
    unittest.main()
