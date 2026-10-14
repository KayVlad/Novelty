import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ge_installer', Path(__file__).parents[1] / 'scripts/install-ge.py')
ge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ge)
VERSION = 'GE-Proton11-7'
CONFIG = dict(schema=1, version=VERSION, auto_download=True)


def archive(version=VERSION, extra=()):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode='w:gz') as package:
        for name, content, mode in [('proton', b'#!/bin/sh\nexit 0\n', 0o755), ('version', version.encode(), 0o644), ('files/data', b'payload', 0o644)]:
            member = tarfile.TarInfo(version + '/' + name)
            member.size, member.mode = len(content), mode
            package.addfile(member, io.BytesIO(content))
        link = tarfile.TarInfo(version + '/files/link')
        link.type, link.linkname = tarfile.SYMTYPE, 'data'
        package.addfile(link)
        for member, data in extra:
            package.addfile(member, io.BytesIO(data))
    return result.getvalue()


def provider(data, version=VERSION, legacy=False, checksum=None, size=None):
    name = version + ('.tar.gz' if legacy else '-x86_64.tar.gz')
    checksum_name = name.removesuffix('.tar.gz') + '.sha512sum'
    prefix = ge.REPOSITORY + '/releases/download/' + version + '/'
    release = dict(tag_name=version, draft=False, assets=[
        dict(name=version + '-aarch64.tar.gz', size=1, browser_download_url=prefix + version + '-aarch64.tar.gz'),
        dict(name=name, size=len(data) if size is None else size, browser_download_url=prefix + name),
        dict(name=checksum_name, size=158, browser_download_url=prefix + checksum_name),
    ])
    responses = {
        ge.API + version: json.dumps(release).encode(),
        prefix + checksum_name: ((checksum or hashlib.sha512(data).hexdigest()) + '  ' + name + '\n').encode(),
        prefix + name: data,
    }

    def open_url(request, timeout=30):
        url = request.full_url if hasattr(request, 'full_url') else request
        if url not in responses:
            raise AssertionError('Unexpected network request: ' + url)
        return io.BytesIO(responses[url])
    return open_url


class GEInstallerTests(unittest.TestCase):
    def test_latest_selector_resolves_release_before_install(self):
        data = archive()
        upstream = provider(data)
        def open_url(request, timeout=30):
            url = request.full_url if hasattr(request, 'full_url') else request
            if url == ge.LATEST_API:
                return io.BytesIO(json.dumps(dict(tag_name=VERSION, draft=False, prerelease=False)).encode())
            return upstream(request, timeout)
        with tempfile.TemporaryDirectory() as directory, patch.object(ge.urllib.request, 'urlopen', side_effect=open_url):
            self.assertEqual(ge.resolve(directory, dict(CONFIG, version='ge-proton-latest')),
                             Path(directory) / '.novelty/runners' / VERSION)

    def test_current_and_legacy_release_assets_select_x86_64(self):
        for version, legacy in [(VERSION, False), ('GE-Proton7-18', True)]:
            data = archive(version)
            with self.subTest(version=version), patch.object(ge.urllib.request, 'urlopen', side_effect=provider(data, version, legacy)):
                info = ge.release_info(version)
                self.assertEqual(info['sha512'], hashlib.sha512(data).hexdigest())
                self.assertNotIn('aarch64', info['asset'])
                self.assertEqual(info['size'], len(data))

    def test_verified_install_offline_reuse_and_side_by_side_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ['GE-Proton10-32', VERSION]:
                data = archive(version)
                with patch.object(ge.urllib.request, 'urlopen', side_effect=provider(data, version)):
                    result = ge.resolve(root, dict(CONFIG, version=version, sha512=hashlib.sha512(data).hexdigest()))
                self.assertEqual(result, root / '.novelty/runners' / version)
                self.assertEqual((result / 'files/link').read_bytes(), b'payload')
                self.assertTrue(os.access(result / 'proton', os.X_OK))
                with patch.object(ge.urllib.request, 'urlopen', side_effect=AssertionError('Offline reuse must not use network')):
                    self.assertEqual(ge.resolve(root, dict(CONFIG, version=version, auto_download=False)), result)
            self.assertTrue((root / '.novelty/runners/GE-Proton10-32/proton').exists())

    def test_disabled_download_and_invalid_configuration_do_not_contact_network(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(ge.urllib.request, 'urlopen', side_effect=AssertionError('No network expected')):
            root = Path(directory)
            for invalid in [dict(CONFIG, auto_download=False), dict(CONFIG, auto_download='false'), dict(CONFIG, version='latest'),
                            dict(CONFIG, version='../escape'), dict(CONFIG, architecture='aarch64'), dict(CONFIG, sha512='bad')]:
                with self.subTest(config=invalid), self.assertRaises((ValueError, RuntimeError)):
                    ge.resolve(root, invalid)
            self.assertFalse((root / '.novelty').exists())

    def test_failed_download_or_pin_keeps_existing_versions(self):
        for mode in ('checksum', 'truncated', 'pin', 'network'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                old = root / '.novelty/runners/GE-Proton10-32'
                old.mkdir(parents=True)
                (old / 'proton').write_bytes(b'working runner')
                data = archive()
                open_url = provider(data, checksum='0' * 128 if mode == 'checksum' else None,
                                    size=len(data) + 10 if mode == 'truncated' else None)
                config = dict(CONFIG, sha512='f' * 128) if mode == 'pin' else CONFIG
                with patch.object(ge.urllib.request, 'urlopen', side_effect=OSError('connection lost') if mode == 'network' else open_url):
                    with self.assertRaises((RuntimeError, OSError)):
                        ge.resolve(root, config)
                self.assertEqual((old / 'proton').read_bytes(), b'working runner')
                self.assertFalse((old.parent / VERSION).exists())
                self.assertFalse((old.parent / ('.install-' + VERSION)).exists())

    def test_archive_paths_links_and_special_files_cannot_escape(self):
        examples = []
        for name in ('../outside', '/tmp/novelty-outside', VERSION + '/../../outside', VERSION + '/novelty-install.json'):
            member = tarfile.TarInfo(name)
            member.size = 1
            examples.append((member, b'x'))
        for kind, target in [(tarfile.SYMTYPE, '../../../outside'), (tarfile.SYMTYPE, '/tmp/outside'), (tarfile.LNKTYPE, '../../outside')]:
            member = tarfile.TarInfo(VERSION + '/files/unsafe-link')
            member.type, member.linkname = kind, target
            examples.append((member, b''))
        member = tarfile.TarInfo(VERSION + '/pipe')
        member.type = tarfile.FIFOTYPE
        examples.append((member, b''))
        for member, contents in examples:
            with self.subTest(name=member.name, kind=member.type), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                data = archive(extra=[(member, contents)])
                with patch.object(ge.urllib.request, 'urlopen', side_effect=provider(data)), self.assertRaises((RuntimeError, tarfile.TarError)):
                    ge.resolve(root, CONFIG)
                self.assertFalse((root / '.novelty/runners' / VERSION).exists())
                self.assertFalse((root / 'outside').exists())

    def test_interrupted_owned_work_restarts_and_published_work_cleans_offline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runners = root / '.novelty/runners'
            work, intent = runners / ('.install-' + VERSION), runners / ('.install-' + VERSION + '.json')
            owner = dict(schema=1, version=VERSION, owner='Novelty GE installer')
            work.mkdir(parents=True)
            (work / 'partial').write_bytes(b'incomplete archive')
            intent.write_text(json.dumps(owner))
            data = archive()
            with patch.object(ge.urllib.request, 'urlopen', side_effect=provider(data)):
                target = ge.resolve(root, CONFIG)
            self.assertFalse(work.exists())
            work.mkdir()
            (work / 'partial').touch()
            intent.write_text(json.dumps(owner))
            with patch.object(ge.urllib.request, 'urlopen', side_effect=AssertionError('No network')):
                self.assertEqual(ge.resolve(root, dict(CONFIG, auto_download=False)), target)
            self.assertFalse(work.exists())
            self.assertFalse(intent.exists())

    def test_concurrent_install_and_unknown_workspace_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runners = root / '.novelty/runners'
            runners.mkdir(parents=True)
            with (runners / ('.' + VERSION + '.install.lock')).open('a+b') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaisesRegex(RuntimeError, 'already being installed'):
                    ge.resolve(root, CONFIG)
            work = runners / ('.install-' + VERSION)
            work.mkdir()
            (work / 'keep').write_text('unrecognized data')
            with self.assertRaisesRegex(RuntimeError, 'Unrecognized'):
                ge.resolve(root, CONFIG)
            self.assertEqual((work / 'keep').read_text(), 'unrecognized data')

    def test_linked_storage_and_changed_receipt_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            games, external = root / 'Games', root / 'External'
            games.mkdir()
            external.mkdir()
            (games / '.novelty').symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(RuntimeError, 'symbolic links'):
                ge.resolve(games, CONFIG)
            self.assertFalse((external / 'runners').exists())
            (games / '.novelty').unlink()
            data = archive()
            with patch.object(ge.urllib.request, 'urlopen', side_effect=provider(data)):
                target = ge.resolve(games, CONFIG)
            receipt = target / 'novelty-install.json'
            before = receipt.read_bytes()
            with self.assertRaises(RuntimeError):
                ge.resolve(games, dict(CONFIG, sha512='0' * 128))
            self.assertEqual(receipt.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
