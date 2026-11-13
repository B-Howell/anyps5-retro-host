# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Packs one system's host release as protocol.md "Finding and checking the host" says.

    python3 tools/package_release.py --version 1.0.0 --system windows-x64 --host <host> --core <core>
        --core-licence <SwanStation's LICENSE> [--core-notices <SwanStation's dep folder>]
        --swanstation-commit <sha> --host-commit <sha> --out <folder>

writes anyps5-retro-host-<version>-<system>.zip and anyps5-retro-host-<version>-<system>.files into
<folder>. The zip holds the host, SwanStation, the two licences, SOURCE.txt and, with --core-notices,
every licence, copying, copyright or notice file under that folder (the libraries SwanStation
builds in) as licenses/swanstation-dep/<path with - for />; the .files lists
every file in the zip as `shasum -a 256` prints it, `<64 hex digits>  <name>`, with `/` between
directories. The zip is the same bytes for the same inputs: entries sorted by name, one fixed
time, and fixed permissions.

    python3 tools/package_release.py --sums <folder> <file>...

writes <folder>/SHA256SUMS for the files, sorted by name as tools/release/sign.sh in AnyPS5-ARM
sorts them (LC_ALL=C sort -k2), so signing it there gives the same SHA256SUMS.

    python3 tools/package_release.py --check <folder>

checks a release folder as a launcher checks a download, without the signature: SHA256SUMS lists
each zip and .files with their digests (and at most one source archive), and every zip holds
exactly the files its .files lists, with those digests.
"""

import argparse
import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYSTEMS = {
    'windows-x64': ('anyps5-retro-host.exe', 'swanstation_libretro.dll'),
    'linux-x64': ('anyps5-retro-host', 'swanstation_libretro.so'),
    'macos-arm64': ('anyps5-retro-host', 'swanstation_libretro.dylib'),
}
VERSION = re.compile(r'^[0-9]+\.[0-9]+\.[0-9]+$')
COMMIT = re.compile(r'^[0-9a-f]{40}$')
TIME = (1980, 1, 1, 0, 0, 0)
EXECUTABLE = 0o100755
PLAIN = 0o100644


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sums_text(entries):
    ordered = sorted(entries, key=lambda entry: entry[0].encode('utf-8'))
    return ''.join(f'{digest}  {name}\n' for name, digest in ordered)


def source_text(version, system, host_commit, swanstation_commit):
    return (
        f'anyps5-retro-host {version} for {system}\n'
        '\n'
        'anyps5-retro-host (GPL-2.0-or-later) is built from\n'
        f'https://github.com/AnyTogether/anyps5-retro-host at commit {host_commit}.\n'
        '\n'
        'swanstation_libretro (GPL-3.0) is SwanStation, unmodified, built from\n'
        f'https://github.com/libretro/swanstation at commit {swanstation_commit},\n'
        'the commit this host release was tested with.\n'
        '\n'
        'The complete source of both, with their submodules, is published beside this zip in the\n'
        f'release as anyps5-retro-host-{version}-source.tar.gz. licenses/ holds the licences of the\n'
        'host, of SwanStation and of the libraries SwanStation builds in.\n'
    )


def release_files(args):
    host_name, core_name = SYSTEMS[args.system]
    host_licence = ROOT / 'LICENSES' / 'GPL-2.0-or-later.txt'
    return [
        (host_name, Path(args.host).read_bytes(), EXECUTABLE),
        (core_name, Path(args.core).read_bytes(), PLAIN),
        ('licenses/anyps5-retro-host-GPL-2.0-or-later.txt', host_licence.read_bytes(), PLAIN),
        ('licenses/swanstation-GPL-3.0.txt', Path(args.core_licence).read_bytes(), PLAIN),
        ('SOURCE.txt',
         source_text(args.version, args.system, args.host_commit, args.swanstation_commit).encode('utf-8'),
         PLAIN),
        *dependency_notices(args.core_notices),
    ]


NOTICE = re.compile(r'^(licen[cs]e|copying|copyright|notice)', re.IGNORECASE)


def dependency_notices(folder):
    if folder is None:
        return []
    root = Path(folder)
    found = sorted(path for path in root.rglob('*') if path.is_file() and NOTICE.match(path.name))
    if not found:
        raise SystemExit(f'--core-notices {folder} holds no licence file')
    return [(f'licenses/swanstation-dep/{"-".join(path.relative_to(root).parts)}', path.read_bytes(), PLAIN)
            for path in found]


def write_zip(path, files):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data, mode in sorted(files, key=lambda entry: entry[0].encode('utf-8')):
            info = zipfile.ZipInfo(name, date_time=TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = mode << 16
            archive.writestr(info, data)


def package(args):
    if args.system not in SYSTEMS:
        raise SystemExit(f'--system must be one of {", ".join(SYSTEMS)}')
    if not VERSION.match(args.version):
        raise SystemExit('--version must be <major>.<minor>.<patch>')
    for name in ('host_commit', 'swanstation_commit'):
        if not COMMIT.match(getattr(args, name)):
            raise SystemExit(f'--{name.replace("_", "-")} must be a full commit hash')
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stem = f'anyps5-retro-host-{args.version}-{args.system}'
    files = release_files(args)
    write_zip(out / f'{stem}.zip', files)
    (out / f'{stem}.files').write_bytes(sums_text([(name, sha256(data)) for name, data, _ in files]).encode('utf-8'))
    print(f'{stem}.zip and {stem}.files written to {out}')


def write_sums(folder, paths):
    entries = []
    seen = set()
    for path in map(Path, paths):
        name = path.name
        if name in ('SHA256SUMS', 'SHA256SUMS.sig'):
            raise SystemExit(f'{path}: the sums do not list themselves')
        if any(character.isspace() for character in name) or name in seen:
            raise SystemExit(f'{path}: a release file name has no spaces and is used once')
        seen.add(name)
        entries.append((name, sha256(path.read_bytes())))
    if not entries:
        raise SystemExit('no file to list')
    out = Path(folder)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'SHA256SUMS').write_bytes(sums_text(entries).encode('utf-8'))
    sys.stdout.write(sums_text(entries))


SOURCE_NAME = re.compile(r'^anyps5-retro-host-[0-9]+\.[0-9]+\.[0-9]+-source\.tar\.gz$')
RELEASE_NAME = re.compile(r'^anyps5-retro-host-([0-9]+\.[0-9]+\.[0-9]+)-([a-z]+-[a-z0-9]+)\.(zip|files)$')


def parse_sums(text, allow_slash):
    entries = {}
    for line in text.splitlines():
        match = re.match(r'^([0-9a-f]{64})  (.+)$', line)
        if not match:
            raise SystemExit(f'cannot read the line {line!r}')
        name = match.group(2)
        if (not allow_slash and '/' in name) or '\\' in name or name in entries:
            raise SystemExit(f'the name {name!r} is not allowed')
        entries[name] = match.group(1)
    return entries


def check(folder):
    folder = Path(folder)
    sums = parse_sums((folder / 'SHA256SUMS').read_text('utf-8'), allow_slash=False)
    stems = {}
    sources = 0
    for name, digest in sums.items():
        actual = sha256((folder / name).read_bytes())
        if actual != digest:
            raise SystemExit(f'{name} does not match SHA256SUMS')
        if SOURCE_NAME.match(name):
            sources += 1
            continue
        match = RELEASE_NAME.match(name)
        if not match or match.group(2) not in SYSTEMS:
            raise SystemExit(f'SHA256SUMS lists {name}, which is not a host release file')
        stems.setdefault(name.rsplit('.', 1)[0], set()).add(match.group(3))
    if not stems:
        raise SystemExit('SHA256SUMS lists no host release')
    if sources > 1:
        raise SystemExit('SHA256SUMS lists more than one source archive')
    for stem, kinds in sorted(stems.items()):
        if kinds != {'zip', 'files'}:
            raise SystemExit(f'{stem} needs both its .zip and its .files')
        listed = parse_sums((folder / f'{stem}.files').read_text('utf-8'), allow_slash=True)
        with zipfile.ZipFile(folder / f'{stem}.zip') as archive:
            held = {info.filename: sha256(archive.read(info)) for info in archive.infolist() if not info.is_dir()}
        if held != listed:
            raise SystemExit(f'{stem}.zip does not hold exactly what {stem}.files lists')
        host_name, core_name = SYSTEMS[RELEASE_NAME.match(f'{stem}.zip').group(2)]
        if host_name not in listed or core_name not in listed:
            raise SystemExit(f'{stem}.files does not list {host_name} and {core_name}')
        print(f'{stem}: {len(listed)} files match')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sums', metavar='FOLDER')
    parser.add_argument('--check', metavar='FOLDER')
    parser.add_argument('--version')
    parser.add_argument('--system')
    parser.add_argument('--host')
    parser.add_argument('--core')
    parser.add_argument('--core-licence')
    parser.add_argument('--core-notices', metavar='FOLDER')
    parser.add_argument('--host-commit')
    parser.add_argument('--swanstation-commit')
    parser.add_argument('--out')
    parser.add_argument('files', nargs='*')
    args = parser.parse_args()
    if args.sums:
        write_sums(args.sums, args.files)
        return
    if args.check:
        check(args.check)
        return
    missing = [name for name in ('version', 'system', 'host', 'core', 'core_licence', 'host_commit',
                                 'swanstation_commit', 'out') if getattr(args, name) is None]
    if missing or args.files:
        parser.error('packing needs --version, --system, --host, --core, --core-licence, --host-commit, '
                     '--swanstation-commit and --out, and no other file')
    package(args)


if __name__ == '__main__':
    main()
