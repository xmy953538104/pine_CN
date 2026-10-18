#!/usr/bin/env python3
import os
import sys
import time
from pathlib import Path
sys.stdout.reconfigure(encoding='utf-8')
with open(os.environ['PINE_FAKE_ADB_ARGS'], 'a') as stream:
    stream.write(' '.join(sys.argv[1:]) + '\n')
mode = os.environ.get('PINE_FAKE_ADB_MODE', '')
if sys.argv[1] == 'devices':
    print('List of devices attached')
    if mode == 'discovery-error':
        print('fake discovery failure', file=sys.stderr)
        sys.exit(7)
    if mode != 'none':
        print('test-device\t' + (mode if mode in ('unauthorized', 'offline') else 'no permissions (check udev rules)' if mode == 'permissions' else 'device'))
        if mode == 'multiple':
            print('second-device\tdevice')
else:
    pid_path = Path(os.environ['PINE_FAKE_ADB_PID'])
    pid_temp = Path(str(pid_path) + '.tmp')
    pid_temp.write_text(str(os.getpid()))
    pid_temp.replace(pid_path)
    print('--------- beginning of main')
    if mode == 'crash-context':
        print('Fatal signal 6 (SIGABRT), controlled fixture only', flush=True)
        print('FATAL EXCEPTION: synthetic thread', flush=True)
    if mode != 'sessions':
        # Only invalid input is synthetic; valid JSONL is replayed unchanged.
        for invalid in ['not JSON', '{"session_id":null}', '{"session_id":5}',
                        '{"session_id":""}', '{"session_id":{"nested":"invalid"}}']:
            print(invalid)
    fixture = os.environ['PINE_FAKE_ADB_SESSIONS_FIXTURE' if mode == 'sessions' else 'PINE_FAKE_ADB_FIXTURE']
    for line in Path(fixture).read_text(encoding='utf-8').splitlines():
        print(line, flush=True)
    if mode == 'sessions':
        sys.exit(0)
    if mode == 'disconnect':
        print('device disconnected', file=sys.stderr)
        sys.exit(1)
    if mode == 'closed-output':
        os.close(sys.stdout.fileno())
        Path(os.environ['PINE_FAKE_ADB_CLOSED']).write_text(str(os.getpid()))
    time.sleep(60)
