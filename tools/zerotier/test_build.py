#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check archive extraction boundaries used by the pinned SDK builder."""
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import build


class ArchiveExtraction(unittest.TestCase):
    def archive(self, root, entries):
        archive = root / "source.tar"
        with tarfile.open(archive, "w") as package:
            info = tarfile.TarInfo("source")
            info.type = tarfile.DIRTYPE
            package.addfile(info)
            for name, kind in entries:
                info = tarfile.TarInfo(name)
                info.type = kind
                if kind == tarfile.SYMTYPE or kind == tarfile.LNKTYPE:
                    info.linkname = "../../outside"
                if kind == tarfile.REGTYPE:
                    info.size = 4
                    package.addfile(info, io.BytesIO(b"data"))
                else:
                    package.addfile(info)
        return archive

    def test_regular_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.archive(root, [("source/nested/file", tarfile.REGTYPE)])
            build.extract(archive, root / "out")
            self.assertEqual((root / "out/nested/file").read_bytes(), b"data")

    def test_untrusted_paths_and_links(self):
        cases = [
            ("source/../../outside", tarfile.REGTYPE),
            ("/outside", tarfile.REGTYPE),
            ("source/C:/outside", tarfile.REGTYPE),
            ("source/dir\\outside", tarfile.REGTYPE),
            ("other-root/outside", tarfile.REGTYPE),
            ("source/link", tarfile.SYMTYPE),
            ("source/link", tarfile.LNKTYPE),
            ("source/device", tarfile.CHRTYPE),
        ]
        for entry in cases:
            with self.subTest(entry=entry), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = self.archive(root, [entry])
                with self.assertRaises(RuntimeError):
                    build.extract(archive, root / "out")
                self.assertFalse((root / "outside").exists())

    def test_duplicate_file_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.archive(root, [("source/file", tarfile.REGTYPE)] * 2)
            with self.assertRaises(FileExistsError):
                build.extract(archive, root / "out")
            self.assertEqual((root / "out/file").read_bytes(), b"data")


if __name__ == "__main__":
    unittest.main()
