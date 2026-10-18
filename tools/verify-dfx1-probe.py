#!/usr/bin/env python3
"""Offline raw/derived DFX1 log consistency check. Never uploads or extracts logs."""
import argparse
import json
from pathlib import Path
import re
import urllib.parse
import zipfile

LOG = re.compile(r'(?:(?:main|emulation)/)?diagnostics-[0-7]\.jsonl')


def read_rows(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            files = [entry for entry in archive.infolist() if LOG.fullmatch(entry.filename)]
            if len({entry.filename for entry in files}) != len(files):
                raise ValueError('Duplicate log names in archive')
            if sum(entry.file_size for entry in files) > 40 * 1024 * 1024:
                raise ValueError('Archive log byte limit exceeded')
            for entry in files:
                if entry.file_size > 8 * 1024 * 1024:
                    raise ValueError('Log file byte limit exceeded')
                yield from parse_rows(archive.read(entry))
    else:
        if path.stat().st_size > 8 * 1024 * 1024:
            raise ValueError('Log file byte limit exceeded')
        yield from parse_rows(path.read_bytes())


def parse_rows(data):
    for line in data.decode('utf-8', errors='strict').splitlines():
        if not line: continue
        if len(line.encode('utf-8')) > 65536: raise ValueError('Record byte limit exceeded')
        row = json.loads(line)
        if not isinstance(row, dict): raise ValueError('Invalid event object')
        if row.get('schema') == 1 and row.get('record_kind', 'event') == 'event' and isinstance(row.get('fields'), dict):
            yield row


def verify(rows, probe, event='DFX_PROBE', source=None, expected_file=None, expected_line=None, expected_message=None):
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', probe): raise ValueError('Invalid probe ID')
    rows = list(rows)
    observed = []
    for row in rows:
        fields = row['fields']
        if row.get('event') != event or fields.get('probe_id') != probe or fields.get('bridge_protocol') != 'DFX1': continue
        if fields.get('protocol_validation') != 'ok' or fields.get('script_fields_source') != 'guest_DFX1_payload': continue
        if fields.get('source') not in ('OutputDebugString', 'LogManager') or (source and fields['source'] != source): continue
        for key, expected in (('game_script_file', expected_file), ('game_script_line', expected_line), ('game_message', expected_message)):
            if expected is not None and fields.get(key) != expected: raise ValueError('Expected guest-provided field mismatch: ' + key)
        raw = [candidate for candidate in rows if candidate.get('event') in ('GUEST_DEBUG_MESSAGE', 'GUEST_RUNTIME_ERROR')
               and candidate.get('session_id') == row.get('session_id') and candidate.get('launch_id') == row.get('launch_id')
               and candidate['fields'].get('source') == fields['source'] and candidate['fields'].get('message') == fields.get('message')]
        if not raw: raise ValueError('Matching original guest message is absent; retained state alone is insufficient')
        wire = fields['message'].rstrip('\r\n')
        if not wire.startswith('DFX1|'): raise ValueError('Original message has no exact DFX1 prefix')
        pairs = [part.split('=', 1) for part in wire[5:].split('|')]
        if any(len(pair) != 2 for pair in pairs) or len({pair[0] for pair in pairs}) != len(pairs): raise ValueError('Malformed original wire record')
        decoded = {key: urllib.parse.unquote(value, errors='strict') for key, value in pairs}
        if decoded.get('probe_id') != probe or decoded.get('event') != event: raise ValueError('Raw/derived probe or event mismatch')
        observed.append((row.get('session_id', ''), row.get('launch_id', ''), fields['source']))
    if not observed: raise ValueError('No matching raw and parsed DFX1 probe; absence does not establish a Lua phase')
    if len(set(observed)) != 1: raise ValueError('Probe ID was reused across sessions/launches/sources; calibrate with a fresh unique ID')
    session, launch, observed_source = observed[0]
    return {'schema': 1, 'status': 'matching_raw_and_parsed_records', 'probe_id': probe, 'event': event,
            'session_id': session, 'launch_id': launch, 'source': observed_source, 'matching_records': len(observed),
            'scope': 'structural log consistency; device provenance and actual game/Lua bridge require controlled on-device calibration',
            'menu_or_world_execution_inferred': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='Local exported ZIP or captured JSONL')
    parser.add_argument('--probe-id', required=True)
    parser.add_argument('--expect-event', default='DFX_PROBE', choices=('DFX_PROBE', 'DFX_MAIN_ENTER', 'DFX_MODINDEX_READY', 'DFX_MODMAIN_OK', 'DFX_HAMLET_ENTER', 'DFX_LUA_ERROR'))
    parser.add_argument('--source', choices=('OutputDebugString', 'LogManager'))
    parser.add_argument('--expect-file'); parser.add_argument('--expect-line'); parser.add_argument('--expect-message')
    args = parser.parse_args()
    try:
        result = verify(read_rows(args.input), args.probe_id, args.expect_event, args.source, args.expect_file, args.expect_line, args.expect_message)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))
