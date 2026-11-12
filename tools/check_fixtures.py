# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'fixtures' / 'v1'
MAX_MESSAGE = 1 << 20

FIELDS = {
    'hello': {'minVersion': int, 'maxVersion': int, 'pixelFormats': list},
    'welcome': {'version': int, 'pid': int, 'hostVersion': str, 'capabilities': list, 'pixelFormat': str},
    'unsupported': {'minVersion': int, 'maxVersion': int, 'pixelFormats': list},
    'call': {'id': int, 'method': str},
    'reply': {'id': int},
    'input': {'port': int, 'buttons': int, 'leftX': int, 'leftY': int, 'rightX': int, 'rightY': int},
    'ping': {'id': int},
    'pong': {'id': int},
    'shutdown': {},
    'ring': {'bytes': int},
    'frame': {},
    'rumble': {'port': int, 'strong': int, 'weak': int},
}
STEP_KINDS = {'hostExits', 'wait', 'launcherKillsHost', 'launcherReports'}
OUTCOMES = {'clean', 'protocolError', 'unsupported', 'hostFailure'}

failures = []


def fail(path, text):
    failures.append(f'{path.relative_to(ROOT).as_posix()}: {text}')


def check_message(path, message, loose=()):
    if not isinstance(message, dict) or not isinstance(message.get('type'), str):
        return fail(path, 'a message must be an object with a string type')
    size = len(json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8'))
    if not 2 <= size <= MAX_MESSAGE:
        fail(path, f'{message["type"]} is {size} bytes')
    fields = FIELDS.get(message['type'])
    if fields is None:
        return
    for name, kind in fields.items():
        value = message.get(name)
        if name in loose and value is None:
            continue
        if not isinstance(value, kind) or (kind is int and isinstance(value, bool)):
            fail(path, f'{message["type"]}.{name} must be {kind.__name__}')
    if message['type'] == 'reply' and ('result' in message) == ('error' in message):
        fail(path, 'a reply has exactly one of result and error')


def check_messages():
    names = set()
    for path in sorted((FIXTURES / 'messages').glob('*.json')):
        names.add(path.stem)
        data = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data.get('description'), str) or data.get('from') not in ('launcher', 'host'):
            fail(path, 'needs a description and from')
        if data.get('os') not in (None, 'windows', 'posix'):
            fail(path, 'os is windows or posix')
        loose = data.get('anyValueFor', [])
        check_message(path, data.get('message'), loose)
        if 'reply' in data:
            check_message(path, data['reply'])
        if data.get('message', {}).get('type') == 'call' and 'reply' not in data:
            fail(path, 'a call needs its reply')
    return names


def check_sessions():
    names = set()
    for path in sorted((FIXTURES / 'sessions').glob('*.json')):
        names.add(path.stem)
        data = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data.get('description'), str):
            fail(path, 'needs a description')
        if not data.get('tests') or not set(data['tests']) <= {'host', 'launcher'}:
            fail(path, 'tests is host and/or launcher')
        if data.get('os') not in (None, 'windows', 'posix', 'linux'):
            fail(path, 'os is windows, posix or linux')
        if data.get('outcome') not in OUTCOMES:
            fail(path, f'outcome is one of {sorted(OUTCOMES)}')
        for index, step in enumerate(data.get('steps', [])):
            where = f'step {index}'
            if 'from' in step:
                if step['from'] not in ('launcher', 'host'):
                    fail(path, f'{where}: from is launcher or host')
                if 'bytes' in step:
                    if not re.fullmatch(r'(?:[0-9a-f]{2})+', step['bytes']):
                        fail(path, f'{where}: bytes is lowercase hex')
                    continue
                check_message(path, step.get('message'), step.get('anyValueFor', []))
                ring = step.get('ring')
                if ring is not None and not (isinstance(ring.get('memoryBytes'), int) and
                                             isinstance(ring.get('sealed'), bool) and
                                             isinstance(ring.get('header'), dict) and
                                             len(ring['header'].get('slots', [])) == 3):
                    fail(path, f'{where}: ring needs memoryBytes, sealed and a header with three slots')
            elif len(step) != 1 or next(iter(step)) not in STEP_KINDS:
                fail(path, f'{where}: unknown step {sorted(step)}')
    return names


def listed(kind):
    text = (ROOT / 'docs' / 'protocol.md').read_text(encoding='utf-8')
    match = re.search(rf'^- {kind}: (.*?)[;.]$', text, re.MULTILINE | re.DOTALL)
    return set(re.findall(r'`([a-z0-9-]+)`', match.group(1))) if match else set()


for kind, found in (('messages', check_messages()), ('sessions', check_sessions())):
    named = listed(kind)
    for name in sorted(found - named):
        failures.append(f'fixtures/v1/{kind}/{name}.json is not listed in docs/protocol.md')
    for name in sorted(named - found):
        failures.append(f'docs/protocol.md lists {kind} fixture {name}, which does not exist')

print('\n'.join(failures) or 'All fixtures are well formed and listed.')
sys.exit(1 if failures else 0)
