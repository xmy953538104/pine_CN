"""Check raw production-sink fixtures, preserving schema and deterministic data."""
import json
import pathlib
import shutil
import sys

fixture_root = pathlib.Path(__file__).resolve().parent / 'fixtures'
generated_root = pathlib.Path(sys.argv[1])
update = sys.argv[2:] == ['--update']


def read(path):
    rows = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
    assert rows, path
    sessions = {}
    for row in rows:
        assert row['schema'] == 1 and isinstance(row['session_id'], str) and row['session_id'], path
        for key in ('sequence', 'wall_clock_ms', 'monotonic_us', 'dropped_count'):
            assert type(row[key]) is int and row[key] >= 0, (path, key)
        assert row['wall_clock_ms'] > 0 and row['monotonic_us'] > 0, path
        assert isinstance(row['thread'], str) and row['thread'], path
        assert isinstance(row['title_id'], str) and isinstance(row['fields'], dict), path
        assert all(isinstance(k, str) and isinstance(v, str) for k, v in row['fields'].items()), path
        previous = sessions.setdefault(row['session_id'], [])
        assert not previous or row['sequence'] > previous[-1]['sequence'], path
        previous.append(row)
    for session_rows in sessions.values():
        summary = session_rows[-1]
        assert summary['event'] == 'SESSION_SUMMARY', path
        assert summary['fields']['accepted'] == summary['fields']['written'] == str(len(session_rows) - 1), path
        assert summary['fields']['disk_errors'] == '0', path
    return rows


def normalized(rows):
    # Keep all keys, value types, event payloads, title context, order, sequence
    # and summary counters. Only clock/thread identity and try-lock scheduling
    # depend on the host/run; those recorded values remain in the tracked raw file.
    return [dict(row, wall_clock_ms=1, monotonic_us=1, thread='runtime', dropped_count=0) for row in rows]


for name, count in [('native-sink.jsonl', 14), ('native-sessions.jsonl', 81)]:
    generated = generated_root / name
    actual = read(generated)
    assert len(actual) == count, (name, len(actual))
    target = fixture_root / name
    if update:
        fixture_root.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(generated, target)
    assert normalized(actual) == normalized(read(target)), (
        f'{name}: production sink changed; inspect then regenerate with '
        'bash tools/tests/check-native-adb-fixtures.sh --update'
    )

capture = read(generated_root / 'native-sink.jsonl')
dlc = next(row for row in capture if row['event'] == 'DLC_DATA_READ')
assert dlc['title_id'] == '0100751007ADA000' and dlc['fields']['title_id'] == '0100751007ADB001'
assert any(row['event'] == 'GUEST_DEBUG_MESSAGE' and row['title_id'] and 'title_id' not in row['fields'] for row in capture)
assert any(row['event'] == 'BASE_SELECTED' and row['fields']['message'] == '\u4e2d\u6587 \u65e5\u5fd7' for row in capture)
probe = next(row for row in capture if row['event'] == 'DFX_PROBE')
assert probe['fields']['bridge_protocol'] == 'DFX1' and probe['fields']['game_message'] == '中文 | %'
assert any(row['event'] == 'GUEST_DEBUG_MESSAGE' and row['fields'].get('message') == probe['fields']['message'] for row in capture)
print('PASS: ADB fixtures replay the current production sink schema, title context and UTF8 output')
