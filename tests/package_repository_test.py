"""Verify signed RPM payloads stay consistent with published repository metadata."""
import gzip
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "rpm_repository", Path(__file__).resolve().parents[1] / "tools/build_rpm_repository.py")
repository = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repository)


class RpmMetadata(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.tree = Path(self.directory.name)
        (self.tree / "repodata").mkdir()
        (self.tree / "packages").mkdir()
        self.package = self.tree / "packages/aino-player.rpm"
        self.package.write_bytes(b"signed RPM payload")
        (self.tree / "repodata/repomd.xml").write_text(
            '<repomd xmlns="http://linux.duke.edu/metadata/repo">'
            '<data type="primary"><location href="repodata/primary.xml.gz"/></data></repomd>')
        self.index()

    def index(self, location="packages/aino-player.rpm"):
        checksum = hashlib.sha256(self.package.read_bytes()).hexdigest()
        content = ('<metadata xmlns="http://linux.duke.edu/metadata/common"><package>'
                   '<location href="' + location + '"/><checksum type="sha256">' + checksum +
                   '</checksum></package></metadata>')
        (self.tree / "repodata/primary.xml.gz").write_bytes(gzip.compress(content.encode()))

    def test_signed_payload_matches_index(self):
        repository.verify_package_checksums(self.tree)

    def test_signing_after_index_is_rejected(self):
        self.package.write_bytes(self.package.read_bytes() + b"new embedded signature")
        with self.assertRaisesRegex(ValueError, "checksum"):
            repository.verify_package_checksums(self.tree)

    def test_unindexed_package_is_rejected(self):
        (self.tree / "packages/extra.rpm").write_bytes(b"unindexed payload")
        with self.assertRaisesRegex(ValueError, "every package"):
            repository.verify_package_checksums(self.tree)

    def test_package_cannot_escape_pool(self):
        self.index("../outside.rpm")
        with self.assertRaisesRegex(ValueError, "escapes"):
            repository.verify_package_checksums(self.tree)


if __name__ == "__main__":
    unittest.main()
