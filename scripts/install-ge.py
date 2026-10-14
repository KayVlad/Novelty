#!/usr/bin/env python3
"""Resolve/install an exact official GE-Proton version from a JSON configuration."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import sys
import tarfile
import urllib.request

REPOSITORY = 'https://github.com/GloriousEggroll/proton-ge-custom'
API = 'https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/tags/'
LATEST_API = 'https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest'
MAX_ARCHIVE = 4 * 1024**3
MAX_EXPANDED = 16 * 1024**3
RESERVE = 64 * 1024**2


def tag(value):
    if not isinstance(value, str) or not re.fullmatch(r'GE-Proton[0-9]+-[0-9]+', value):
        raise ValueError('Choose an exact GE-Proton release tag, such as GE-Proton11-7; latest is not a pin')
    return value


def small_download(url, limit):
    request = urllib.request.Request(url, headers={'User-Agent': 'Novelty-GE-installer/1'})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise RuntimeError('Release metadata exceeds its size limit')
    return data


def release_info(version):
    version = tag(version)
    release = json.loads(small_download(API + version, 2 * 1024**2))
    if release.get('tag_name') != version or release.get('draft'):
        raise RuntimeError('Upstream release does not match the configured tag')
    assets = release.get('assets', [])
    archive = None
    for name in (version + '-x86_64.tar.gz', version + '.tar.gz'):
        matches = [a for a in assets if a.get('name') == name]
        if len(matches) > 1:
            raise RuntimeError('Ambiguous release assets')
        if matches:
            archive = matches[0]
            break
    if archive is None:
        raise RuntimeError('This release has no supported x86-64 archive')
    name = archive['name']
    checksum_name = name.removesuffix('.tar.gz') + '.sha512sum'
    sums = [a for a in assets if a.get('name') == checksum_name]
    if len(sums) != 1:
        raise RuntimeError('Release must provide exactly one matching SHA-512 checksum asset')
    prefix = REPOSITORY + '/releases/download/' + version + '/'
    if archive.get('browser_download_url') != prefix + name or sums[0].get('browser_download_url') != prefix + checksum_name:
        raise RuntimeError('Unexpected upstream download location')
    size = archive.get('size')
    if type(size) is not int or not 0 < size <= MAX_ARCHIVE:
        raise RuntimeError('Release archive size is invalid or exceeds the download limit')
    expected = []
    for line in small_download(prefix + checksum_name, 8192).decode('ascii').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{128})[ \t]+\*?(.+)', line)
        if match and match[2] in (name, './' + name):
            expected.append(match[1].lower())
    if len(expected) != 1:
        raise RuntimeError('Checksum file does not uniquely identify the selected archive')
    return dict(version=version, architecture='x86_64', asset=name, url=prefix + name, size=size, sha512=expected[0])


def latest_tag():
    release = json.loads(small_download(LATEST_API, 2 * 1024**2))
    if release.get('draft') or release.get('prerelease'):
        raise RuntimeError('The latest GE release is not stable')
    return tag(release.get('tag_name'))


def configuration(value):
    if not isinstance(value, dict) or type(value.get('schema')) is not int or value['schema'] != 1:
        raise ValueError('Unsupported GE configuration schema')
    version = value.get('version')
    if version != 'ge-proton-latest':
        version = tag(version)
    if type(value.get('auto_download')) is not bool:
        raise ValueError('auto_download must be true or false')
    if value.get('architecture', 'x86_64') != 'x86_64':
        raise ValueError('This installer targets the x86-64 Steam Deck')
    checksum = value.get('sha512')
    if checksum is not None and (not isinstance(checksum, str) or not re.fullmatch(r'[0-9a-fA-F]{128}', checksum)):
        raise ValueError('sha512 must be a 128-character checksum when provided')
    return version, value['auto_download'], checksum.lower() if checksum else None


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def verify_installed(target, version, pinned):
    if target.is_symlink() or not target.is_dir():
        raise RuntimeError('Existing runner path is not a managed directory')
    receipt = json.loads((target / 'novelty-install.json').read_text())
    if (receipt.get('schema') != 1 or receipt.get('version') != version or receipt.get('architecture') != 'x86_64'
            or not isinstance(receipt.get('sha512'), str) or not re.fullmatch(r'[0-9a-f]{128}', receipt['sha512'])
            or (pinned and receipt['sha512'] != pinned)
            or not (target / 'proton').is_file() or not os.access(target / 'proton', os.X_OK)
            or not (target / 'version').is_file()):
        raise RuntimeError('Existing runner does not match the configured installation; it was not overwritten')
    return target


def clear_owned_workspace(work, intent, owner):
    if work.exists() or work.is_symlink():
        if work.is_symlink() or intent.is_symlink() or not intent.is_file() or json.loads(intent.read_text()) != owner:
            raise RuntimeError('Unrecognized installation workspace; files were kept for inspection')
        shutil.rmtree(work)
    if intent.exists():
        if intent.is_symlink() or json.loads(intent.read_text()) != owner:
            raise RuntimeError('Unrecognized installation intent; kept for inspection')
        intent.unlink()


def extract_archive(archive, payload, version):
    if not hasattr(tarfile, 'data_filter'):
        raise RuntimeError('GE extraction requires Python with tarfile.data_filter (Python 3.12+ recommended)')
    payload.mkdir()
    with tarfile.open(archive, mode='r:gz') as package:
        members = package.getmembers()
        if len(members) > 200000 or sum(max(m.size, 0) for m in members) > MAX_EXPANDED:
            raise RuntimeError('GE archive exceeds extraction limits')
        if shutil.disk_usage(payload).free < sum(max(m.size, 0) for m in members) + RESERVE:
            raise RuntimeError('Not enough free space to extract GE-Proton')
        roots = set()
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts or not path.parts:
                raise RuntimeError('Unsafe archive path')
            if path.parts[0] not in (version, version + '-x86_64'):
                raise RuntimeError('Archive does not contain the configured GE release directory')
            if len(path.parts) == 2 and path.parts[1] in ('novelty-install.json', 'novelty-install.json.tmp'):
                raise RuntimeError('Archive contains reserved installer metadata')
            roots.add(path.parts[0])
        if len(roots) != 1:
            raise RuntimeError('Expected a single GE release directory')
        root = payload / roots.pop()

        def safe_member(member, destination):
            filtered = tarfile.data_filter(member, destination)
            if filtered.issym() or filtered.islnk():
                link = ((payload / filtered.name).parent / filtered.linkname if filtered.issym()
                        else payload / filtered.linkname)
                if not link.resolve().is_relative_to(root):
                    raise RuntimeError('Archive link escapes the GE release directory')
            return filtered

        package.extractall(payload, members=members, filter=safe_member)
    if (root.is_symlink() or not (root / 'proton').is_file() or not os.access(root / 'proton', os.X_OK)
            or not (root / 'version').is_file()):
        raise RuntimeError('Extracted archive is missing the Proton launcher/version')
    return root


def resolve(games_folder, config):
    version, auto_download, pinned = configuration(config)
    if version == 'ge-proton-latest':
        if not auto_download or pinned:
            raise ValueError('A latest GE selector needs downloads enabled and cannot use a fixed checksum pin')
        version = latest_tag()
    if platform.machine() not in ('x86_64', 'AMD64'):
        raise RuntimeError('This installer currently supports x86-64 hosts only')
    root = Path(games_folder).resolve(strict=True)
    if not root.is_dir():
        raise ValueError('Games folder must be a directory')
    runners = root / '.novelty/runners'
    if runners.resolve() != runners:
        raise RuntimeError('Managed runner directory must not contain symbolic links')
    target = runners / version
    if not (target.exists() or target.is_symlink()) and not auto_download:
        raise RuntimeError(f'{version} is not installed and automatic downloads are disabled')
    runners.mkdir(parents=True, exist_ok=True)
    with (runners / ('.' + version + '.install.lock')).open('a+b') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('This GE version is already being installed') from error
        work = runners / ('.install-' + version)
        intent = runners / ('.install-' + version + '.json')
        owner = dict(schema=1, version=version, owner='Novelty GE installer')
        if target.exists() or target.is_symlink():
            installed = verify_installed(target, version, pinned)
            clear_owned_workspace(work, intent, owner)
            return installed  # Offline reuse, including cleanup after interrupted publication.
        clear_owned_workspace(work, intent, owner)
        atomic_json(intent, owner)  # Authorize this exact workspace before creating it.
        work.mkdir()
        try:
            info = release_info(version)
            if pinned and info['sha512'] != pinned:
                raise RuntimeError('Upstream checksum differs from the configuration pin')
            if shutil.disk_usage(work).free < info['size'] + RESERVE:
                raise RuntimeError('Not enough free space to download GE-Proton')
            archive = work / 'release.tar.gz'
            digest = hashlib.sha512()
            total = 0
            request = urllib.request.Request(info['url'], headers={'User-Agent': 'Novelty-GE-installer/1'})
            with urllib.request.urlopen(request, timeout=30) as response, archive.open('wb') as stream:
                while block := response.read(1024**2):
                    total += len(block)
                    if total > info['size']:
                        raise RuntimeError('Download exceeds the published archive size')
                    digest.update(block)
                    stream.write(block)
            if total != info['size'] or digest.hexdigest() != info['sha512']:
                raise RuntimeError('GE download is truncated or its SHA-512 checksum is incorrect')
            extracted = extract_archive(archive, work / 'payload', version)
            atomic_json(extracted / 'novelty-install.json', dict(schema=1, **info))
            # Same-filesystem publication; never replace an existing version.
            if target.exists() or target.is_symlink():
                raise RuntimeError('Runner destination appeared during installation; it was kept')
            os.rename(extracted, target)
        finally:
            shutil.rmtree(work)
            intent.unlink(missing_ok=True)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('games_folder', type=Path, nargs='?')
    parser.add_argument('--config', type=Path)
    parser.add_argument('--inspect-release', metavar='TAG', help='Fetch release/checksum metadata only, without installing')
    args = parser.parse_args()
    if args.inspect_release:
        print(json.dumps(release_info(args.inspect_release), indent=2))
        return
    if args.games_folder is None or args.config is None:
        parser.error('games_folder and --config are required to resolve/install a runner')
    print(resolve(args.games_folder, json.loads(args.config.read_text())))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, tarfile.TarError) as error:
        print(f'GE-Proton installation failed: {error}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('GE-Proton installation cancelled.', file=sys.stderr)
        sys.exit(130)
