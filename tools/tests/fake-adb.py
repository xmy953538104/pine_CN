#!/usr/bin/env python3
import json
import os
import sys
import time
from pathlib import Path
with open(os.environ['PINE_FAKE_ADB_ARGS'], 'a') as stream:
    stream.write(' '.join(sys.argv[1:]) + '\n')
mode = os.environ.get('PINE_FAKE_ADB_MODE', '')
if sys.argv[1] == 'devices':
    print('List of devices attached')
    if mode != 'none':
        print('test-device\t' + (mode if mode in ('unauthorized', 'offline') else 'device'))
        if mode == 'multiple':
            print('second-device\tdevice')
else:
    Path(os.environ['PINE_FAKE_ADB_PID']).write_text(str(os.getpid()))
    print('--------- beginning of main')
    print(json.dumps(dict(session_id='session-a', title_id='0100', message='中文 日志'), ensure_ascii=False), flush=True)
    print(json.dumps(dict(session_id='session-b', title_id='0200', message='other')), flush=True)
    if mode == 'disconnect':
        print('device disconnected', file=sys.stderr)
        sys.exit(1)
    time.sleep(60)
