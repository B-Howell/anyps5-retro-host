# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

"""Checks that tools/package_release.py names in SOURCE.txt the repository and commit the host is
really built from: HEAD of the checkout and the public https URL of the remote it tracks, with no
user name, password or token, or GitHub Actions' GITHUB_REPOSITORY at GITHUB_SHA without a
checkout, and that it refuses a checkout with changes that are not committed, a checkout git cannot
read, a local remote and a build it cannot name, and does not take another repository's HEAD for an
unpacked source inside it. Each case runs in a new git repository in a temporary folder, with the
user's and the system's git configuration left out."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import package_release as p  # noqa: E402

os.environ['GIT_CONFIG_GLOBAL'] = os.devnull
os.environ['GIT_CONFIG_NOSYSTEM'] = '1'
FORK = 'https://github.com/example-fork/anyps5-retro-host'
CI = {
    'GITHUB_ACTIONS': 'true',
    'GITHUB_SERVER_URL': 'https://github.com',
    'GITHUB_REPOSITORY': 'example-ci/anyps5-retro-host',
    'GITHUB_SHA': '0123456789abcdef0123456789abcdef01234567',
}
failures = []


def check(name, got, want):
    print(f'{name}: {got!r}')
    if got != want:
        failures.append(f'{name}: got {got!r}, want {want!r}')


def outcome(*arguments, **keywords):
    try:
        return p.host_source(*arguments, **keywords)
    except SystemExit as refusal:
        return f'refused: {refusal}'


def refused(text, *fragments):
    return isinstance(text, str) and text.startswith('refused: ') and all(part in text for part in fragments)


def git(root, *arguments):
    subprocess.run(['git', '-C', str(root), '-c', 'user.name=test', '-c', 'user.email=test@example.invalid',
                    '-c', 'commit.gpgsign=false', *arguments], check=True, capture_output=True)


def repository(folder, remotes=(), branch='work', upstream=None):
    root = Path(folder)
    root.mkdir(parents=True)
    git(root, 'init', '-q', '-b', branch)
    (root / 'host.txt').write_text('host\n')
    git(root, 'add', 'host.txt')
    git(root, 'commit', '-q', '-m', 'host')
    for name, url in remotes:
        git(root, 'remote', 'add', name, url)
    if upstream:
        git(root, 'config', f'branch.{branch}.remote', upstream)
    return root, p.git(root, 'rev-parse', 'HEAD')


remotes = [
    ('https', 'https://github.com/example-fork/anyps5-retro-host.git', FORK),
    ('https with a token', 'https://x-access-token:secret@github.com/example-fork/anyps5-retro-host', FORK),
    ('https with a port and a slash', 'https://git.example.org:8443/team/host/', 'https://git.example.org:8443/team/host'),
    ('scp-like ssh', 'git@github.com:example-fork/anyps5-retro-host.git', FORK),
    ('ssh with a port', 'ssh://git@GitHub.com:22/example-fork/anyps5-retro-host.git', FORK),
    ('git protocol', 'git://github.com/example-fork/anyps5-retro-host', FORK),
    ('a POSIX path', '/srv/git/anyps5-retro-host', None),
    ('a Windows path', 'C:\\work\\anyps5-retro-host', None),
    ('a Windows path with slashes', 'C:/work/anyps5-retro-host', None),
    ('a UNC path', '\\\\server\\share\\anyps5-retro-host', None),
    ('a relative path', '../anyps5-retro-host', None),
    ('file://', 'file:///srv/git/anyps5-retro-host', None),
    ('plain http', 'http://github.com/example-fork/anyps5-retro-host', None),
    ('no path', 'https://github.com/', None),
    ('IPv6 https', 'https://[2001:db8::1]:8443/team/host.git', 'https://[2001:db8::1]:8443/team/host'),
    ('IPv6 ssh', 'ssh://git@[2001:db8::1]/team/host', 'https://[2001:db8::1]/team/host'),
    ('a port that is no number', 'https://github.com:http/team/host', None),
    ('an ssh port that is no number', 'ssh://git@github.com:http/team/host', None),
    ('an IPv6 host without its closing bracket', 'https://[2001:db8::1/team/host', None),
]
for name, remote, want in remotes:
    check(f'public URL of {name}', p.public_url(remote), want)

with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)

    root, head = repository(folder / 'tracked', [('origin', 'https://github.com/example-upstream/anyps5-retro-host'),
                                                 ('fork', 'git@github.com:example-fork/anyps5-retro-host.git')],
                            upstream='fork')
    check('the remote the branch tracks, at HEAD', outcome(root, environment=CI), (FORK, head))
    text = p.source_text('1.0.0', 'linux-x64', *p.host_source(root, environment={}), 'f' * 40)
    check('SOURCE.txt names them', f'{FORK} at commit {head}.' in text.splitlines(), True)
    check('SOURCE.txt names no other host repository', 'AnyTogether' in text, False)

    root, head = repository(folder / 'origin', [('mirror', 'https://mirror.example.org/anyps5-retro-host'),
                                                ('origin', 'https://github.com/example-fork/anyps5-retro-host')])
    check('origin when the branch tracks nothing', outcome(root, environment={}), (FORK, head))

    root, head = repository(folder / 'single', [('fork', 'https://github.com/example-fork/anyps5-retro-host')])
    check('the only remote', outcome(root, environment={}), (FORK, head))

    root, head = repository(folder / 'detached', [('origin', 'https://github.com/example-fork/anyps5-retro-host')])
    git(root, 'checkout', '-q', '--detach')
    check('a detached HEAD uses origin', outcome(root, environment={}), (FORK, head))

    root, head = repository(folder / 'token', [('origin', 'https://user:secret@github.com/example-fork/anyps5-retro-host')])
    found = outcome(root, environment={})
    check('a token in the remote is left out', found, (FORK, head))
    check('no secret in SOURCE.txt', 'secret' in p.source_text('1.0.0', 'linux-x64', *found, 'f' * 40), False)

    root, head = repository(folder / 'local', [('origin', str(folder / 'tracked'))])
    found = outcome(root, environment=CI)
    check('a local remote is refused, not written', refused(found, '--host-repository'), True)
    check('the refusal does not name the path', str(folder / 'tracked') in str(found), False)
    check('a local remote with --host-repository', outcome(root, FORK + '.git', environment={}), (FORK, head))

    root, head = repository(folder / 'none')
    check('a checkout with no remote in GitHub Actions', outcome(root, environment=CI),
          ('https://github.com/example-ci/anyps5-retro-host', head))
    check('a checkout with no remote elsewhere', refused(outcome(root, environment={}), '--host-repository'), True)

    root, head = repository(folder / 'dirty', [('origin', FORK)])
    (root / 'host.txt').write_text('changed\n')
    check('changes that are not committed', refused(outcome(root, environment={}), 'not committed'), True)
    git(root, 'checkout', '-q', '--', 'host.txt')
    (root / 'build.log').write_text('untracked\n')
    check('an untracked file is not a change', outcome(root, environment={}), (FORK, head))

    check('--host-commit HEAD', outcome(root, commit=head, environment={}), (FORK, head))
    check('--host-commit not HEAD', refused(outcome(root, commit='a' * 40, environment={}), 'HEAD'), True)

    broken = folder / 'broken'
    broken.mkdir()
    (broken / '.git').write_text('not a git file\n')
    check('a checkout git cannot read is refused, not left to GitHub Actions',
          refused(outcome(broken, environment=CI), 'failed in the checkout'), True)

    plain = folder / 'unpacked-source'
    plain.mkdir()
    check('no checkout in GitHub Actions', outcome(plain, environment=CI),
          ('https://github.com/example-ci/anyps5-retro-host', CI['GITHUB_SHA']))
    check('no checkout, named by hand', outcome(plain, FORK, 'b' * 40, environment={}), (FORK, 'b' * 40))
    check('no checkout, nothing named', refused(outcome(plain, environment={}), '--host-repository'), True)
    nested = folder / 'tracked' / 'unpacked-source'
    nested.mkdir()
    check('an unpacked source inside another checkout', refused(outcome(nested, environment={}), '--host-repository'),
          True)
    check('no checkout, no commit', refused(outcome(plain, FORK, environment={}), '--host-commit'), True)
    check('no checkout, a local --host-repository', refused(outcome(plain, str(folder), 'b' * 40, environment={}),
                                                            '--host-repository'), True)
    check('GitHub Enterprise', outcome(plain, environment={**CI, 'GITHUB_SERVER_URL': 'https://git.example.org/'}),
          ('https://git.example.org/example-ci/anyps5-retro-host', CI['GITHUB_SHA']))

print('FAILED: ' + '; '.join(failures) if failures else 'package_release passed')
sys.exit(1 if failures else 0)
