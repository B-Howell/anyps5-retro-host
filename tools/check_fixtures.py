# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

import copy
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'fixtures' / 'v1'
MAX_MESSAGE = 1 << 20
ID = (1, 2**31 - 1)
U32 = (1, 2**32 - 1)
AXIS = (-32768, 32767)
NONE = 0xFFFFFFFF
MAX_SLOT = 268435456
STEPS = {'hostExits', 'pollWithin', 'silence', 'wait', 'launcherKillsHost', 'launcherDraws', 'launcherReports'}
OUTCOMES = {'clean', 'protocolError', 'unsupported', 'hostFailure'}
SENDERS = {'hello': 'launcher', 'call': 'launcher', 'input': 'launcher', 'ping': 'launcher', 'shutdown': 'launcher',
           'welcome': 'host', 'unsupported': 'host', 'reply': 'host', 'pong': 'host', 'ring': 'host',
           'frame': 'host', 'rumble': 'host'}


class Bad(Exception):
    pass


class BadArgument(Bad):
    pass


def need(condition, text):
    if not condition:
        raise Bad(text)


def unique_keys(pairs):
    keys = [key for key, _ in pairs]
    need(len(keys) == len(set(keys)), 'a key appears twice in one object')
    return dict(pairs)


def no_lone_surrogates(value):
    if isinstance(value, str):
        need(not re.search('[\ud800-\udfff]', value), 'a string holds a lone surrogate')
    elif isinstance(value, list):
        for entry in value:
            no_lone_surrogates(entry)
    elif isinstance(value, dict):
        for key, entry in value.items():
            no_lone_surrogates(key)
            no_lone_surrogates(entry)


def not_json(name):
    raise ValueError(f'{name} is not JSON')


def parse(text):
    value = json.loads(text, object_pairs_hook=unique_keys, parse_constant=not_json)
    no_lone_surrogates(value)
    return value


def is_number(value):
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        return abs(value) < 2**1024
    return isinstance(value, float) and math.isfinite(value)


def integer(value, low, high, name):
    need(is_number(value) and (isinstance(value, int) or value.is_integer()) and low <= value <= high,
         f'{name} must be an integer from {low} to {high}')


def boolean(value, name):
    need(isinstance(value, bool), f'{name} must be a boolean')


def string(value, name):
    need(isinstance(value, str), f'{name} must be a string')


def strings(value, name):
    need(isinstance(value, list) and all(isinstance(entry, str) for entry in value), f'{name} must be a list of strings')


def obj(value, name):
    need(isinstance(value, dict), f'{name} must be an object')
    return value


def none(value, name=None):
    need(value is None, f'{name} must be null')


PATH_OS = {'os': None}


def path_parts(value):
    posix = value.startswith('/') and not value.startswith('//')
    need(posix or PATH_OS['os'] in (None, 'windows'), 'not an absolute path')
    need(not posix or PATH_OS['os'] != 'windows', 'not an absolute path')
    if posix:
        separator, rest = '/', value[1:]
    else:
        long_form = re.match(r'\\\\\?\\(?:UNC\\(?P<server>[^\\]+)\\(?P<share>[^\\]+)|[A-Za-z]:(?=\\))', value)
        short_form = re.match(r'[\\/]{2}(?P<server>[^\\/?.][^\\/]*)[\\/](?P<share>[^\\/]+)|[A-Za-z]:(?=[\\/])', value)
        match = long_form or short_form
        need(match, 'not an absolute path')
        separator, rest = (r'\\' if long_form else r'[\\/]'), value[match.end():]
        need(rest == '' or re.match(separator, rest), 'not an absolute path')
        rest = rest[1:]
        names = [name for name in (match.groupdict().get('server'), match.groupdict().get('share')) if name]
        return names + ([] if rest == '' else re.split(separator, rest))
    return [] if rest == '' else re.split(separator, rest)


def path(value, name):
    string(value, name)
    if re.fullmatch(r'\$\{[a-z0-9]+\}', value or ''):
        return
    try:
        parts = path_parts(value)
        need('\0' not in value and all(part not in ('', '.', '..') for part in parts), 'bad segment')
    except Bad:
        raise Bad(f'{name} must be an absolute path or ${{name}}')


def extension(value, *names):
    return value.lower().endswith(names)


def start_args(args, name=None):
    obj(args, 'start args')
    for field in ('core', 'disc', 'system', 'saves'):
        path(args.get(field), f'start.{field}')
    if 'options' in args:
        for key, value in obj(args['options'], 'start.options').items():
            string(value, f'start.options.{key}')
    if 'initialDisc' in args:
        integer(args['initialDisc'], 0, 127, 'start.initialDisc')
        need(extension(args['disc'], '.m3u') or args['disc'] == '${playlist}', 'initialDisc needs an .m3u disc')
    elif args['disc'] not in ('${disc}', '${playlist}', '${disc1}', '${disc2}'):
        need(extension(args['disc'], '.cue', '.chd', '.pbp', '.m3u'), 'start.disc is a .cue, .chd, .pbp or .m3u')


def status_result(value, name=None, report=False):
    if value is None:
        return
    obj(value, 'status')
    boolean(value.get('ready'), 'status.ready')
    boolean(value.get('ended'), 'status.ended')
    string(value.get('error'), 'status.error')
    string(value.get('errorCode'), 'status.errorCode')
    if report and set(value) == {'ready', 'ended', 'error', 'errorCode'}:
        need(value['ended'] and not value['ready'], 'a lost game is ended and not ready')
        return
    need(is_number(value.get('aspect')) and 0 < value['aspect'] <= 10, 'status.aspect must be a number above 0, at most 10')
    for field in ('hardware', 'graphicsPending'):
        boolean(value.get(field), f'status.{field}')
    string(value.get('resolutionError'), 'status.resolutionError')
    need('progressPercent' in value and 'progressLabel' in value, 'status needs progressLabel and progressPercent')
    need(value['progressLabel'] is None or isinstance(value['progressLabel'], str), 'status.progressLabel')
    if value['progressPercent'] is not None:
        integer(value['progressPercent'], 0, 100, 'status.progressPercent')
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
        strings(labels, 'discs.labels')
        if discs.get('inserted') is not None:
            integer(discs['inserted'], 0, len(labels) - 1, 'discs.inserted')
        need('inserted' in discs and 'insertedDiscPath' in discs, 'discs needs inserted and insertedDiscPath')
        need(discs['insertedDiscPath'] is None or isinstance(discs['insertedDiscPath'], str), 'discs.insertedDiscPath')
        string(discs.get('playedDiscPath'), 'discs.playedDiscPath')
        boolean(discs.get('busy'), 'discs.busy')
        boolean(discs.get('failed'), 'discs.failed')


def volume_args(args, name=None):
    obj(args, 'volume args')
    need(is_number(args.get('level')) and 0 <= args['level'] <= 1, 'volume.level must be a number from 0 to 1')
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


def check_args_of(message):
    try:
        METHODS[message['method']][0](message.get('args'), f'{message["method"]} args')
    except Bad as problem:
        raise BadArgument(str(problem))


def check_message(message, calls, check_args=True):
    obj(message, 'message')
    kind = message.get('type')
    string(kind, 'type')
    size = len(json.dumps(message, separators=(',', ':'), ensure_ascii=False).encode('utf-8'))
    need(2 <= size <= MAX_MESSAGE, f'{kind} is {size} bytes')
    if kind in ('hello', 'unsupported'):
        integer(message.get('minVersion'), *U32, 'minVersion')
        integer(message.get('maxVersion'), message['minVersion'], U32[1], 'maxVersion')
        strings(message.get('pixelFormats'), 'pixelFormats')
    elif kind == 'welcome':
        integer(message.get('version'), *U32, 'version')
        integer(message.get('pid'), *U32, 'pid')
        string(message.get('hostVersion'), 'hostVersion')
        strings(message.get('capabilities'), 'capabilities')
        string(message.get('pixelFormat'), 'pixelFormat')
    elif kind == 'call':
        integer(message.get('id'), *ID, 'id')
        string(message.get('method'), 'method')
        calls[message['id']] = message['method']
        if message['method'] in METHODS and check_args:
            check_args_of(message)
    elif kind == 'reply':
        integer(message.get('id'), *ID, 'id')
        need(('result' in message) != ('error' in message), 'a reply has exactly one of result and error')
        method = calls.get(message['id'])
        if 'error' in message:
            error = obj(message['error'], 'error')
            string(error.get('code'), 'error.code')
            string(error.get('message'), 'error.message')
        elif method in METHODS:
            METHODS[method][1](message['result'], f'{method} result')
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


def check_slot(slot, slot_bytes):
    obj(slot, 'slot')
    integer(slot.get('width'), 1, 8192, 'frame width')
    integer(slot.get('height'), 1, 8192, 'frame height')
    need(slot['width'] * slot['height'] * 4 <= slot_bytes, 'frame fits its slot')
    need(slot.get('visible') in (0, 1), 'visible')
    integer(slot.get('epoch'), *ID, 'slot epoch')
    integer(slot.get('sequence'), 1, 2**63 - 1, 'slot sequence')


def check_ring(ring, message, state):
    need(is_number(ring.get('memoryBytes')) and float(ring['memoryBytes']).is_integer() and
         isinstance(ring.get('sealed'), bool), 'ring memory')
    header = obj(ring.get('header'), 'ring.header')
    page = 16384 if state['os'] == 'macos' else 1
    need(0 <= ring['memoryBytes'] - message['bytes'] < page,
         'the memory is bytes long')
    formats = {'rgba8': 1, 'bgra8': 2}
    need(ring['sealed'] or state['os'] in ('windows', 'macos'), 'Linux rings are sealed')
    need(header.get('magic') == 0x46523541 and header.get('version') == 1 and header.get('slotCount') == 3 and
         header.get('headerBytes') == 4096 and header.get('pixelFormat') in (1, 2), 'ring header constants')
    need(header['pixelFormat'] == formats.get(state['pixelFormat']), 'the ring has the pixel format of welcome')
    slot_bytes = header.get('slotBytes')
    integer(slot_bytes, 4, MAX_SLOT, 'slotBytes')
    need(slot_bytes % 4 == 0, 'slotBytes is a multiple of 4')
    need(message['bytes'] == 4096 + 3 * slot_bytes, 'bytes is 4096 + 3 x slotBytes')
    kept = state['slotBytes']
    if kept is not None:
        need(slot_bytes > kept and slot_bytes >= min(2 * kept, MAX_SLOT), 'a new ring is at least twice as large')
    slots = header.get('slots')
    need(isinstance(slots, list) and len(slots) == 3, 'three slots')
    latest = header.get('latest')
    need(latest in (0, 1, 2, NONE) and header.get('reading') in (0, 1, 2, NONE), 'latest and reading')
    state['slotBytes'] = slot_bytes
    state['slots'] = [dict(slot) for slot in slots]
    state['latest'] = latest


def check_frame(step, state):
    need(state['slotBytes'] is not None, 'a frame needs a ring')
    if 'publish' in step:
        publish = obj(step['publish'], 'publish')
        need(publish.get('latest') in (0, 1, 2), 'publish.latest is 0, 1 or 2')
        state['slots'][publish['latest']] = obj(publish.get('slot'), 'publish.slot')
        state['latest'] = publish['latest']
    if state['latest'] != NONE:
        check_slot(state['slots'][state['latest']], state['slotBytes'])
        state['shown'] = state['slots'][state['latest']]['epoch']


def check_entry(path, where, run):
    try:
        run()
    except Bad as problem:
        failures.append(f'{path.relative_to(ROOT).as_posix()}: {where}: {problem}')


def load(path):
    try:
        return parse(path.read_text(encoding='utf-8'))
    except Bad as problem:
        failures.append(f'{path.relative_to(ROOT).as_posix()}: {problem}')
        return {}


def argument_error(reply):
    need(isinstance(reply.get('error'), dict) and reply['error'].get('code') == 'argument',
         'a call with a wrong argument gets an argument error')


def check_messages():
    for path in sorted((FIXTURES / 'messages').glob('*.json')):
        data = load(path)

        def run():
            string(data.get('description'), 'description')
            need(data.get('from') in ('launcher', 'host'), 'from is launcher or host')
            need(data.get('os') in (None, 'windows', 'posix'), 'os is windows or posix')
            PATH_OS['os'] = data.get('os')
            calls = {}
            wrong_argument = False
            try:
                check_message(data.get('message'), calls)
                need('invalid' not in data, f'marked invalid ({data.get("invalid")}) but breaks no rule')
            except Bad as problem:
                if data.get('invalid') != str(problem):
                    raise
                wrong_argument = isinstance(problem, BadArgument)
            calls[data['message'].get('id')] = data['message'].get('method')
            need(SENDERS.get(data['message']['type'], data['from']) == data['from'], 'sender')
            if data['message']['type'] in ('call', 'ping'):
                need('reply' in data, 'a call or ping needs its reply')
            if 'reply' in data:
                check_message(data['reply'], calls)
                need(data['reply']['id'] == data['message']['id'], 'reply id matches')
                if wrong_argument:
                    argument_error(data['reply'])
            if data['message']['type'] == 'ring':
                need(('handle' in data['message']) == (data.get('os') == 'windows'), 'handle exactly on Windows')
        check_entry(path, 'file', run)


LIMITS = {'welcome': 10, 'answer': 5, 'stop': 10, 'shutdown': 15}


def order(message, sender, calls, state, step):
    kind = message.get('type')
    need(SENDERS.get(kind, sender) == sender, f'{kind} is sent by the {SENDERS.get(kind)}')
    if sender == 'launcher':
        if not state['hello']:
            need(kind == 'hello', 'the first message is hello')
            state['hello'] = message
            state['helloAt'] = state['clock']
            return
        need(kind != 'hello', 'a second hello')
        need(state['answer'] == 'welcome', 'nothing but hello before welcome')
    else:
        need(state['hello'], 'the host sends nothing before hello')
        if state['answer'] is None:
            need(kind in ('welcome', 'unsupported'), 'the first host message is welcome or unsupported')
            need(state['clock'] - state['helloAt'] <= LIMITS['welcome'], 'welcome comes within its limit')
            if kind == 'welcome':
                hello = state['hello']
                need(hello['minVersion'] <= message['version'] <= hello['maxVersion'] and
                     message['pixelFormat'] in hello['pixelFormats'], 'welcome picks what hello offered')
                need(not step.get('wrongPid'), 'the welcome pid is the host process id')
                state['pixelFormat'] = message['pixelFormat']
            state['answer'] = kind
            return
        need(state['answer'] == 'welcome', 'nothing after unsupported')
        need(kind not in ('welcome', 'unsupported'), 'a second answer to hello')
    clock = state['clock']
    if kind == 'call':
        need(message.get('id') not in state['called'], 'call ids are unique')
        need(state['called'] or message.get('method') == 'volume', 'the first call after welcome is volume')
        state['called'].add(message.get('id'))
        state['waiting'].append(('call', message.get('id'), message.get('method'), clock))
        if message.get('method') == 'stop':
            state['stopping'] = clock
    elif kind == 'ping':
        need(message.get('id') not in state['pinged'], 'ping ids are unique')
        state['pinged'].add(message.get('id'))
        state['waiting'].append(('ping', message.get('id'), None, clock))
    elif kind in ('reply', 'pong'):
        expected = 'call' if kind == 'reply' else 'ping'
        oldest = state['waiting'][0] if state['waiting'] else None
        need(oldest is not None and oldest[0] == expected,
             'replies and pongs come in the order of their calls and pings')
        need(oldest[1] == message.get('id'), f'a {kind} answers the oldest waiting {expected}')
        need(clock - oldest[3] <= LIMITS['answer'], f'a {kind} comes within its limit')
        state['waiting'].pop(0)
        result = message.get('result')
        if kind == 'reply' and oldest[2] == 'start' and isinstance(result, dict):
            state['epoch'] = result.get('epoch')
        if kind == 'reply' and oldest[2] == 'status' and isinstance(result, dict) and result.get('ended'):
            state['stopping'] = None
    elif kind == 'shutdown':
        state['shutdown'] = clock


def overdue(state):
    clock = state['clock']
    if state['shutdown'] is not None and clock - state['shutdown'] > LIMITS['shutdown']:
        return True
    if state['hello'] and state['answer'] is None and clock - state['helloAt'] > LIMITS['welcome']:
        return True
    if any(clock - item[3] > LIMITS['answer'] for item in state['waiting']):
        return True
    return state['stopping'] is not None and clock - state['stopping'] > LIMITS['stop']


def check_bytes(raw):
    need(re.fullmatch(r'(?:[0-9a-f]{2})+', raw), 'bytes is lowercase hex')
    data = bytes.fromhex(raw)
    need(len(data) >= 4, 'a length is 4 bytes')
    length = int.from_bytes(data[:4], 'little')
    need(2 <= length <= MAX_MESSAGE, 'a length is from 2 to 1 MiB')
    body = data[4:4 + length]
    if len(body) < length:
        return
    try:
        text = body.decode('utf-8')
    except UnicodeDecodeError:
        raise Bad('a body is UTF-8')
    try:
        value = parse(text)
    except ValueError:
        raise Bad('a body is JSON')
    obj(value, 'a body')
    string(value.get('type'), 'type')
    return value


def check_step(step, data, calls, state):
    if 'from' not in step:
        kinds = [key for key in step if key in STEPS]
        need(len(kinds) == 1, f'unknown step {sorted(step)}')
        kind = kinds[0]
        if kind in ('wait', 'silence'):
            need(set(step) <= {'wait', 'status'} if kind == 'wait' else len(step) == 1, f'unknown step {sorted(step)}')
            need(is_number(step[kind]) and step[kind] > 0, f'{kind} is a number of seconds')
            if kind == 'wait':
                status = step.get('status')
                status_result(status)
                need(not any(state['clock'] - item[3] > LIMITS['answer'] for item in state['waiting']),
                     'a reply comes within its limit')
                need(all(item[0] == 'ping' or item[2] == 'status' for item in state['waiting']),
                     'a wait answers only pings and status calls')
                need(not any(item[1] in state['wrongArgs'] for item in state['waiting'] if item[0] == 'call'),
                     'a call with a wrong argument gets an argument error')
                state['waiting'] = []
                if isinstance(status, dict) and status.get('ended'):
                    state['stopping'] = None
            state['clock'] += step[kind]
            return
        elif kind == 'launcherKillsHost':
            need(step['launcherKillsHost'] is True and len(step) == 1, 'launcherKillsHost')
            need(overdue(state), 'the launcher kills the host only after a limit has passed')
        elif kind == 'launcherReports':
            report = obj(step['launcherReports'], 'launcherReports')
            method = report.get('method')
            need(method in METHODS and 'result' in report, 'launcherReports names a method and its result')
            if method == 'status':
                status_result(report['result'], report=True)
            else:
                METHODS[method][1](report['result'], f'{method} result')
        elif kind == 'launcherDraws':
            boolean(step['launcherDraws'], 'launcherDraws')
            need(state['shown'] is not None, 'launcherDraws follows a frame')
            need(step['launcherDraws'] == (state['shown'] == state['epoch']),
                 'the launcher draws exactly the frames of the current game')
        else:
            need(len(step) == 1, f'unknown step {sorted(step)}')
        return
    need(step['from'] in ('launcher', 'host'), 'from is launcher or host')
    if 'bytes' in step:
        message = check_bytes(step['bytes'])
        if message is None:
            return
    else:
        message = obj(step.get('message'), 'message')
    check_message(message, calls, check_args=False)
    order(message, step['from'], calls, state, step)
    if message['type'] == 'call' and message['method'] in METHODS:
        try:
            check_args_of(message)
        except BadArgument:
            state['wrongArgs'].add(message['id'])
            raise
    if message['type'] == 'reply' and message['id'] in state['wrongArgs']:
        argument_error(message)
    windows = state['os'] == 'windows'
    descriptors = step.get('descriptors', 1 if message['type'] == 'ring' and not windows else 0)
    if message['type'] == 'ring':
        need(descriptors == (0 if windows else 1), 'a ring carries one descriptor')
        need(('handle' in message) == windows, 'a ring has a handle exactly on Windows')
        check_ring(obj(step.get('ring'), 'ring'), message, state)
    else:
        need(descriptors == 0, 'descriptors only on ring')
    if 'publish' in step:
        need(message['type'] == 'frame' and step['from'] == 'host', 'publish goes with a host frame')
    if message['type'] == 'frame':
        check_frame(step, state)
    if 'wrongPid' in step:
        need(message['type'] == 'welcome', 'wrongPid goes with welcome')


def check_sessions():
    for path in sorted((FIXTURES / 'sessions').glob('*.json')):
        data = load(path)

        def head():
            string(data.get('description'), 'description')
            need(data.get('tests') and set(data['tests']) <= {'host', 'launcher'}, 'tests is host and/or launcher')
            need(data.get('os') in (None, 'windows', 'posix', 'linux', 'macos'), 'os is windows, posix, linux or macos')
            need(data.get('outcome') in OUTCOMES, f'outcome is one of {sorted(OUTCOMES)}')
            need(isinstance(data.get('steps'), list) and data['steps'], 'steps')
        check_entry(path, 'file', head)
        calls = {}
        state = new_state()
        state['os'] = data.get('os')
        PATH_OS['os'] = 'windows' if data.get('os') == 'windows' else 'posix' if data.get('os') else None
        for index, step in enumerate(data.get('steps', [])):
            before = copy.deepcopy((calls, state))
            try:
                check_step(step, data, calls, state)
                need('invalid' not in step, f'marked invalid ({step.get("invalid")}) but breaks no rule')
            except Bad as problem:
                if not isinstance(problem, BadArgument):
                    calls, state = before
                if step.get('invalid') != str(problem):
                    failures.append(f'{path.relative_to(ROOT).as_posix()}: step {index}: {problem}')


def new_state():
    return {'hello': None, 'answer': None, 'pixelFormat': None, 'waiting': [], 'called': set(), 'pinged': set(),
            'slotBytes': None, 'slots': None, 'latest': NONE, 'shown': None, 'epoch': None, 'stopping': None,
            'shutdown': None, 'clock': 0, 'helloAt': 0, 'os': None, 'wrongArgs': set()}


failures = []
check_messages()
PATH_OS['os'] = None
check_sessions()
print('\n'.join(failures) or 'All fixtures follow the protocol.')
sys.exit(1 if failures else 0)
