# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Computes the core build identity docs/netplay.md "Core build identity" defines, independently of
the build, so CI can check what each system's SwanStation or LRPS2 reports in status.core.

    python3 tools/core_identity.py --swanstation-commit <sha> [--manifest]
    python3 tools/core_identity.py --lrps2-commit <sha> [--manifest]

prints anyps5-1-<SHA-256 of the manifest>, or with --manifest the manifest itself: the source part
the core's patch step writes (the core's commit and each of its patches in cores/patches, in the
order of their names, with its SHA-256: cores/apply-swanstation-patches.sh for SwanStation,
cores/prepare-lrps2.cmake for LRPS2) and the build options the core's identity patch adds, at the
values the host builds it with (SwanStation's sixth patch; LRPS2's first, docs/ps2.md).

    python3 tools/core_identity.py --same <file>... [--swanstation-commit <sha> | --lrps2-commit <sha>]

checks that the status.core files tests/real_core.py or tests/ps2_core.py wrote on each system
(--core-identity-out) are all present and alike, and prints the one identity they hold.
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
# The build options each core's identity patch adds to the manifest, in the order it adds them.
OPTIONS = {
    'swanstation': ('MSB_FIRST=OFF',),
    'lrps2': ('C89_EMITTER=ON', 'DISABLE_ADVANCE_SIMD=ON', 'USE_OPENGL=ON', 'USE_VULKAN=ON'),
}


def manifest(commit, core='swanstation'):
    if not COMMIT.match(commit):
        raise SystemExit(f'--{core}-commit must be a full commit hash')
    patches = sorted(PATCHES.glob(f'{core}-*.patch'), key=lambda path: path.name.encode('utf-8'))
    if not patches:
        raise SystemExit(f'no {core} patches in {PATCHES}')
    lines = ['anyps5-retro-host core source 1', f'{core} {commit}']
    for patch in patches:
        data = patch.read_bytes()
        if b'\r' in data:
            raise SystemExit(f'{patch.name} holds a carriage return; the patches are LF in every checkout')
        lines.append(f'patch {hashlib.sha256(data).hexdigest()} {patch.name}')
    lines.extend(f'option {option}' for option in OPTIONS[core])
    return ''.join(line + '\n' for line in lines)


def identity(commit, core='swanstation'):
    return 'anyps5-1-' + hashlib.sha256(manifest(commit, core).encode('utf-8')).hexdigest()


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
    commits = parser.add_mutually_exclusive_group()
    commits.add_argument('--swanstation-commit')
    commits.add_argument('--lrps2-commit')
    parser.add_argument('--manifest', action='store_true')
    parser.add_argument('--same', nargs='+', metavar='FILE')
    options = parser.parse_args()
    core, commit = ('lrps2', options.lrps2_commit) if options.lrps2_commit else ('swanstation', options.swanstation_commit)
    if options.same:
        value = same(options.same)
        if commit and value.get('version') != identity(commit, core):
            raise SystemExit(f'they report {value.get("version")}, not {identity(commit, core)}')
        return 0
    if not commit:
        parser.error('--swanstation-commit or --lrps2-commit is needed')
    text = manifest(commit, core) if options.manifest else identity(commit, core) + '\n'
    sys.stdout.buffer.write(text.encode('utf-8'))
    return 0



if __name__ == '__main__':
    sys.exit(main())
