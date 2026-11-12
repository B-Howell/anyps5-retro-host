# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'fixtures' / 'v1'
MAX_MESSAGE = 1 << 20
ID = (1, 2**31 - 1)
U32 = (1, 2**32 - 1)
AXIS = (-32768, 32767)
FORMATS = {'rgba8', 'bgra8'}
ERRORS = {'unknownMethod', 'argument', 'busy', 'start'}
STEPS = {'hostExits', 'pollWithin', 'silence', 'launcherKillsHost', 'launcherReports'}
OUTCOMES = {'clean', 'protocolError', 'unsupported', 'hostFailure'}
MAX_SLOT = 268435456


class Bad(Exception):
    pass


def need(condition, text):
    if not condition:
        raise Bad(text)


def integer(value, low, high, name):
    need(isinstance(value, int) and not isinstance(value, bool) and low <= value <= high,
         f'{name} must be an integer from {low} to {high}')


def boolean(value, name):
    need(isinstance(value, bool), f'{name} must be a boolean')


def string(value, name):
    need(isinstance(value, str), f'{name} must be a string')


def path(value, name):
    string(value, name)
    need('\0' not in value and (re.fullmatch(r'\$\{[a-z0-9]+\}', value) or value.startswith('/') or
                                re.match(r'[A-Za-z]:\\', value)), f'{name} must be an absolute path or ${{name}}')


def obj(value, name):
    need(isinstance(value, dict), f'{name} must be an object')
    return value


def formats(value, name):
    need(isinstance(value, list) and value and all(entry in FORMATS for entry in value),
         f'{name} must be a non-empty list of rgba8 and bgra8')


def none(value, name):
    need(value is None, f'{name} must be null')


def start_args(args, name=None):
    obj(args, 'start args')
    for field in ('core', 'disc', 'system', 'saves'):
        path(args.get(field), f'start.{field}')
    if 'internalResolution' in args:
        string(args['internalResolution'], 'start.internalResolution')
    if 'options' in args:
        for key, value in obj(args['options'], 'start.options').items():
            string(value, f'start.options.{key}')
    if 'initialDisc' in args:
        need(args['disc'].endswith('.m3u') or args['disc'] == '${playlist}', 'initialDisc needs an .m3u disc')
        disc = obj(args['initialDisc'], 'start.initialDisc')
        integer(disc.get('index'), 0, 127, 'initialDisc.index')
        path(disc.get('path'), 'initialDisc.path')


def status_result(value, name=None):
    if value is None:
        return
    obj(value, 'status')
    boolean(value.get('ready'), 'status.ready')
    boolean(value.get('ended'), 'status.ended')
    string(value.get('error'), 'status.error')
    if len(value) == 3:
        return
    need(isinstance(value.get('aspect'), (int, float)) and not isinstance(value.get('aspect'), bool),
         'status.aspect must be a number')
    for field in ('hardware', 'graphicsPending'):
        boolean(value.get(field), f'status.{field}')
    string(value.get('resolutionError'), 'status.resolutionError')
    need(value.get('progressLabel') is None or isinstance(value['progressLabel'], str), 'status.progressLabel')
    if value.get('progressPercent') is not None:
        integer(value['progressPercent'], 0, 100, 'status.progressPercent')
    need('progressPercent' in value and 'progressLabel' in value, 'status needs progressLabel and progressPercent')
    for key, option in obj(value.get('options'), 'status.options').items():
        obj(option, f'option {key}')
        string(option.get('title'), f'{key}.title')
        values, labels = option.get('values'), option.get('labels')
        need(isinstance(values, list) and values and all(isinstance(entry, str) for entry in values), f'{key}.values')
        need(isinstance(labels, list) and len(labels) == len(values) and all(isinstance(e, str) for e in labels),
             f'{key}.labels must match values')
        integer(option.get('selected'), 0, len(values) - 1, f'{key}.selected')
        need(option.get('activeValue') in values, f'{key}.activeValue must be one of values')
        boolean(option.get('restart'), f'{key}.restart')
    if 'discs' in value:
        discs = obj(value['discs'], 'status.discs')
        labels = discs.get('labels')
        need(isinstance(labels, list) and all(isinstance(entry, str) for entry in labels), 'discs.labels')
        if discs.get('inserted') is not None:
            integer(discs['inserted'], 0, len(labels) - 1, 'discs.inserted')
        need(discs.get('insertedDiscPath') is None or isinstance(discs['insertedDiscPath'], str),
             'discs.insertedDiscPath')
        string(discs.get('playedDiscPath'), 'discs.playedDiscPath')
        boolean(discs.get('busy'), 'discs.busy')
        boolean(discs.get('failed'), 'discs.failed')


def volume_args(args, name=None):
    obj(args, 'volume args')
    level = args.get('level')
    need(isinstance(level, (int, float)) and not isinstance(level, bool) and 0 <= level <= 1,
         'volume.level must be a number from 0 to 1')
    boolean(args.get('muted'), 'volume.muted')


def option_args(args, name=None):
    obj(args, 'option args')
    string(args.get('key'), 'option.key')
    string(args.get('value'), 'option.value')


def epoch_result(value, name=None):
    integer(obj(value, 'start result').get('epoch'), *ID, 'start.epoch')


def is_bool(value, name='result'):
    boolean(value, name)


METHODS = {
    'start': (start_args, epoch_result),
    'status': (none, status_result),
    'pause': (boolean, none),
    'connectedPorts': (lambda value, name: integer(value, 1, 2, name), is_bool),
    'changeDisc': (lambda value, name: integer(value, 0, 127, name), is_bool),
    'option': (option_args, is_bool),
    'volume': (volume_args, none),
    'stop': (none, none),
    'release': (none, none),
}


def check_message(message, calls):
    obj(message, 'message')
    kind = message.get('type')
    string(kind, 'type')
    size = len(json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8'))
    need(2 <= size <= MAX_MESSAGE, f'{kind} is {size} bytes')
    if kind in ('hello', 'unsupported'):
        integer(message.get('minVersion'), 1, U32[1], 'minVersion')
        integer(message.get('maxVersion'), message['minVersion'], U32[1], 'maxVersion')
        formats(message.get('pixelFormats'), 'pixelFormats')
    elif kind == 'welcome':
        integer(message.get('version'), 1, U32[1], 'version')
        integer(message.get('pid'), *U32, 'pid')
        string(message.get('hostVersion'), 'hostVersion')
        need(isinstance(message.get('capabilities'), list) and
             all(isinstance(entry, str) for entry in message['capabilities']), 'capabilities')
        need(message.get('pixelFormat') in FORMATS, 'pixelFormat')
    elif kind == 'call':
        integer(message.get('id'), *ID, 'id')
        string(message.get('method'), 'method')
        calls[message['id']] = message['method']
        if message['method'] in METHODS:
            METHODS[message['method']][0](message.get('args'), f'{message["method"]} args')
    elif kind == 'reply':
        integer(message.get('id'), *ID, 'id')
        need(('result' in message) != ('error' in message), 'a reply has exactly one of result and error')
        method = calls.get(message['id'])
        if 'error' in message:
            error = obj(message['error'], 'error')
            need(error.get('code') in ERRORS, 'error.code')
            string(error.get('message'), 'error.message')
        elif method in METHODS:
            METHODS[method][1](message['result'], f'{method} result')
        need(method is not None, 'a reply needs an earlier call with its id')
    elif kind == 'input':
        integer(message.get('port'), 0, 1, 'port')
        integer(message.get('buttons'), 0, 65535, 'buttons')
        for axis in ('leftX', 'leftY', 'rightX', 'rightY'):
            integer(message.get(axis), *AXIS, axis)
    elif kind in ('ping', 'pong'):
        integer(message.get('id'), *ID, 'id')
    elif kind == 'ring':
        integer(message.get('bytes'), 4096 + 12, 4096 + 3 * MAX_SLOT, 'bytes')
        if 'handle' in message:
            integer(message['handle'], *U32, 'handle')
    elif kind == 'rumble':
        integer(message.get('port'), 0, 1, 'port')
        integer(message.get('strong'), 0, 65535, 'strong')
        integer(message.get('weak'), 0, 65535, 'weak')


def check_ring(ring, message):
    need(isinstance(ring.get('memoryBytes'), int) and isinstance(ring.get('sealed'), bool), 'ring memory')
    header = obj(ring.get('header'), 'ring.header')
    need(header.get('magic') == 0x46523541 and header.get('version') == 1 and header.get('slotCount') == 3 and
         header.get('headerBytes') == 4096 and header.get('pixelFormat') in (1, 2), 'ring header constants')
    slot_bytes = header.get('slotBytes')
    integer(slot_bytes, 4, MAX_SLOT, 'slotBytes')
    need(slot_bytes % 4 == 0, 'slotBytes is a multiple of 4')
    need(isinstance(message.get('bytes'), int) and message['bytes'] == 4096 + 3 * slot_bytes == ring['memoryBytes'], 'bytes is 4096 + 3 x slotBytes')
    need(ring['sealed'], 'Linux rings are sealed')
    slots = header.get('slots')
    need(isinstance(slots, list) and len(slots) == 3, 'three slots')
    latest = header.get('latest')
    need(latest in (0, 1, 2, 0xFFFFFFFF) and header.get('reading') in (0, 1, 2, 0xFFFFFFFF), 'latest and reading')
    if latest != 0xFFFFFFFF:
        slot = slots[latest]
        integer(slot.get('width'), 1, 8192, 'frame width')
        integer(slot.get('height'), 1, 8192, 'frame height')
        need(slot['width'] * slot['height'] * 4 <= slot_bytes, 'frame fits its slot')
        need(slot.get('visible') in (0, 1), 'visible')
        integer(slot.get('epoch'), *ID, 'slot epoch')
        integer(slot.get('sequence'), 1, 2**63 - 1, 'slot sequence')


def check_entry(path, where, run):
    try:
        run()
    except Bad as problem:
        failures.append(f'{path.relative_to(ROOT).as_posix()}: {where}: {problem}')


def check_messages():
    for path in sorted((FIXTURES / 'messages').glob('*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))

        def run():
            string(data.get('description'), 'description')
            need(data.get('from') in ('launcher', 'host'), 'from is launcher or host')
            need(data.get('os') in (None, 'windows', 'posix'), 'os is windows or posix')
            calls = {}
            check_message(data.get('message'), calls)
            if data['message']['type'] in ('call', 'ping'):
                need('reply' in data, 'a call or ping needs its reply')
            if 'reply' in data:
                check_message(data['reply'], calls)
                need(data['reply']['id'] == data['message']['id'], 'reply id matches')
            if data['message']['type'] == 'ring':
                need(('handle' in data['message']) == (data.get('os') == 'windows'), 'handle exactly on Windows')
        check_entry(path, 'file', run)


def check_sessions():
    for path in sorted((FIXTURES / 'sessions').glob('*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))

        def head():
            string(data.get('description'), 'description')
            need(data.get('tests') and set(data['tests']) <= {'host', 'launcher'}, 'tests is host and/or launcher')
            need(data.get('os') in (None, 'windows', 'posix', 'linux'), 'os is windows, posix or linux')
            need(data.get('outcome') in OUTCOMES, f'outcome is one of {sorted(OUTCOMES)}')
            need(isinstance(data.get('steps'), list) and data['steps'], 'steps')
        check_entry(path, 'file', head)
        calls = {}
        for index, step in enumerate(data.get('steps', [])):
            def run():
                if 'from' not in step:
                    need(len(step) == 1 and next(iter(step)) in STEPS, f'unknown step {sorted(step)}')
                    return
                need(step['from'] in ('launcher', 'host'), 'from is launcher or host')
                if 'bytes' in step:
                    need(re.fullmatch(r'(?:[0-9a-f]{2})+', step['bytes']), 'bytes is lowercase hex')
                    length = int.from_bytes(bytes.fromhex(step['bytes'])[:4], 'little')
                    need(2 <= length <= MAX_MESSAGE, 'length at most 1 MiB')
                    return
                if obj(step.get('message'), 'message').get('type') == 'ring':
                    check_ring(obj(step.get('ring'), 'ring'), step['message'])
                check_message(step['message'], calls)
                descriptors = step.get('descriptors', 1 if step['message']['type'] == 'ring' else 0)
                need(descriptors == (1 if step['message']['type'] == 'ring' else 0), 'descriptors only on ring')
            try:
                run()
                need('invalid' not in step, f'marked invalid ({step.get("invalid")}) but breaks no rule')
            except Bad as problem:
                if step.get('invalid') != str(problem):
                    failures.append(f'{path.relative_to(ROOT).as_posix()}: step {index}: {problem}')


failures = []
check_messages()
check_sessions()
print('\n'.join(failures) or 'All fixtures follow the protocol.')
sys.exit(1 if failures else 0)
