import fcntl
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("installer", Path(__file__).parents[1] / "scripts/install-umu.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerTests(unittest.TestCase):
    def test_latest_release_selector_uses_verified_metadata(self):
        version = '9.8.7'
        name = f'umu-launcher-{version}-zipapp.tar'
        url = f'https://github.com/Open-Wine-Components/umu-launcher/releases/download/{version}/{name}'
        release = dict(tag_name=version, draft=False, prerelease=False, assets=[dict(
            name=name, browser_download_url=url, size=123, digest='sha256:' + 'a' * 64)])
        with patch.object(installer.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(release).encode())):
            self.assertEqual(installer.release_spec('umu-latest'), (version, url, 'a' * 64, None))
        release['assets'][0]['digest'] = ''
        with patch.object(installer.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(release).encode())):
            with self.assertRaisesRegex(RuntimeError, 'SHA-256'):
                installer.release_spec('umu-latest')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.games = self.root / 'Games'
        self.games.mkdir()
        self.payload = b'#!/usr/bin/env python3\n'
        self.archive = self.root / 'release.tar'
        self.make_archive()
        self.target = self.games / f'.novelty/tools/umu-{installer.VERSION}/umu-run'
        for name, value in [('URL', self.archive.as_uri()), ('ZIPAPP_SHA256', hashlib.sha256(self.payload).hexdigest())]:
            mock = patch.object(installer, name, value)
            mock.start()
            self.addCleanup(mock.stop)

    def make_archive(self, names=('release/umu-run', '../../unwanted'), symlink=False):
        with tarfile.open(self.archive, 'w') as tar:
            for name in names:
                member = tarfile.TarInfo(name)
                member.size = len(self.payload)
                if symlink:
                    member.type = tarfile.SYMTYPE
                    member.linkname = '/outside'
                tar.addfile(member, io.BytesIO(self.payload) if not symlink else None)

    def install(self):
        with patch.object(installer, 'SHA256', hashlib.sha256(self.archive.read_bytes()).hexdigest()):
            return installer.install(self.games)

    def test_verified_install_and_offline_reuse(self):
        self.assertEqual(self.install(), self.target)
        self.assertEqual(self.target.read_bytes(), self.payload)
        self.assertTrue(self.target.stat().st_mode & 0o100)
        self.assertFalse((self.root / 'unwanted').exists())
        with patch.object(installer.urllib.request, 'urlopen', side_effect=AssertionError('offline')):
            self.assertEqual(installer.install(self.games), self.target)
        self.assertEqual(list(self.target.parent.parent.glob('.umu-*-*')), [])

    def test_differing_existing_tool_and_legacy_are_preserved(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(b'keep existing version')
        legacy = self.target.parent.parent / 'umu-run'
        legacy.write_bytes(b'keep legacy')
        with self.assertRaisesRegex(RuntimeError, 'not overwritten'):
            self.install()
        self.assertEqual(self.target.read_bytes(), b'keep existing version')
        self.target.unlink()
        self.target.parent.rmdir()
        self.install()
        self.assertEqual(legacy.read_bytes(), b'keep legacy')

    def test_failed_archive_or_payload_checksum_publishes_nothing(self):
        with patch.object(installer, 'SHA256', 'wrong'):
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                installer.install(self.games)
        with patch.object(installer, 'ZIPAPP_SHA256', 'wrong'):
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                self.install()
        with patch.object(installer, 'LIMIT', 2):
            with self.assertRaisesRegex(RuntimeError, 'size limit'):
                self.install()
        self.assertFalse(self.target.parent.exists())

    def test_archive_ambiguity_links_and_missing_executable(self):
        for names, symlink in [(('a/umu-run', 'b/umu-run'), False), (('umu-run',), True), (('other',), False)]:
            with self.subTest(names=names, symlink=symlink):
                self.make_archive(names, symlink)
                with self.assertRaisesRegex(RuntimeError, 'exactly one regular'):
                    self.install()
                self.assertFalse(self.target.parent.exists())

    def test_linked_storage_and_executable_are_rejected(self):
        outside = self.root / 'outside'
        outside.mkdir()
        managed = self.games / '.novelty'
        managed.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, 'linked'):
            self.install()
        self.assertEqual(list(outside.iterdir()), [])
        managed.unlink()
        self.target.parent.mkdir(parents=True)
        payload = outside / 'umu-run'
        payload.write_bytes(self.payload)
        payload.chmod(0o755)
        self.target.symlink_to(payload)
        with self.assertRaisesRegex(RuntimeError, 'not overwritten'):
            self.install()
        self.assertEqual(payload.read_bytes(), self.payload)

    def test_concurrent_install_is_rejected(self):
        tools = self.target.parent.parent
        tools.mkdir(parents=True)
        with (tools / f'.umu-{installer.VERSION}.lock').open('wb') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError, 'already being installed'):
                self.install()
        self.install()


if __name__ == '__main__':
    unittest.main()
