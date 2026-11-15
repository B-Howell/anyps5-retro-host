# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Computes the core build identity docs/netplay.md "Core build identity" defines, independently of
the build, so CI can check what each system's SwanStation reports in status.core.

    python3 tools/core_identity.py --swanstation-commit <sha> [--manifest]

prints anyps5-1-<SHA-256 of the manifest>, or with --manifest the manifest itself: the source part
cores/apply-swanstation-patches.sh writes (the SwanStation commit and each patch in cores/patches,
in the order of their names, with its SHA-256) and the build options the sixth patch adds, at the
values the workflows build with.

    python3 tools/core_identity.py --same <file>...

checks that the status.core files tests/real_core.py wrote on each system (--core-identity-out) are
all present and alike, and prints the one identity they hold.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCHES = ROOT / 'cores' / 'patches'
COMMIT = re.compile(r'^[0-9a-f]{40}$')
OPTIONS = ('MSB_FIRST=OFF',)


def manifest(commit):
    if not COMMIT.match(commit):
        raise SystemExit('--swanstation-commit must be a full commit hash')
    patches = sorted(PATCHES.glob('swanstation-*.patch'), key=lambda path: path.name.encode('utf-8'))
    if not patches:
        raise SystemExit(f'no patches in {PATCHES}')
    lines = ['anyps5-retro-host core source 1', f'swanstation {commit}']
    for patch in patches:
        data = patch.read_bytes()
        if b'\r' in data:
            raise SystemExit(f'{patch.name} holds a carriage return; the patches are LF in every checkout')
        lines.append(f'patch {hashlib.sha256(data).hexdigest()} {patch.name}')
    lines.extend(f'option {option}' for option in OPTIONS)
    return ''.join(line + '\n' for line in lines)


def identity(commit):
    return 'anyps5-1-' + hashlib.sha256(manifest(commit).encode('utf-8')).hexdigest()


def same(paths):
    reported = {}
    for path in map(Path, paths):
        reported[path.name] = json.loads(path.read_text(encoding='utf-8'))
    if len(reported) < 2:
        raise SystemExit(f'only {len(reported)} identities to compare')
    distinct = {json.dumps(value, sort_keys=True) for value in reported.values()}
    for name, value in sorted(reported.items()):
        print(f'{name}: {value}')
    if len(distinct) != 1:
        raise SystemExit('the systems report different core build identities')
    return next(iter(reported.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--swanstation-commit')
    parser.add_argument('--manifest', action='store_true')
    parser.add_argument('--same', nargs='+', metavar='FILE')
    options = parser.parse_args()
    if options.same:
        value = same(options.same)
        if options.swanstation_commit and value.get('version') != identity(options.swanstation_commit):
            raise SystemExit(f'they report {value.get("version")}, not {identity(options.swanstation_commit)}')
        return 0
    if not options.swanstation_commit:
        parser.error('--swanstation-commit is needed')
    text = manifest(options.swanstation_commit) if options.manifest else identity(options.swanstation_commit) + '\n'
    sys.stdout.buffer.write(text.encode('utf-8'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
