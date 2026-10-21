#!/usr/bin/env python3
"""Collector mocks + actual host fork/child-crash/deadline; NOT Android proof."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
subprocess.run(['node', '--check', str(ROOT / 'tools/android-bionic/run-once.js')], check=True)
subprocess.run(['node', '--test', str(ROOT / 'tools/tests/android-bionic.test.js')], check=True)
with tempfile.TemporaryDirectory(prefix='pine-supervisor-test-') as temporary:
    out = Path(temporary)
    parent = out / 'supervisor'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-DPINE_BOUNDARY_PARENT_TEST',
                    '-I' + str(ROOT / 'tests/runtime_observer'), str(ROOT / 'tests/runtime_observer/android_supervisor.cpp'), '-o', str(parent)], check=True)
    for mode, code, names, expected_signal in [('pass', 0, 2, 0), ('crash', 134, 1, 6), ('timeout', 142, 1, 14), ('missing', 124, 1, 0)]:
        folder = out / mode;folder.mkdir()
        child = '''#!/usr/bin/env python3
import os,sys,json,time,signal,resource
from pathlib import Path
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
with open('attempts.txt','a') as file:file.write(Path(sys.argv[0]).name+'\\n')
mode=MODE
if mode=='crash':os.abort()
if mode=='timeout':time.sleep(5)
if mode=='missing':sys.exit(0)
root=Path(sys.argv[1]);root.mkdir()
(root/'component-result.json').write_text(json.dumps({'status':'PASS','android_execution':False}))
'''.replace('MODE', repr(mode))
        for name in ('pine-lifecycle', 'pine-registers'):
            file = folder / name;file.write_text(child);file.chmod(0o700)
        result = subprocess.run([str(parent), 'a' * 32], cwd=folder, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        assert result.returncode == code, (mode, result.returncode, result.stderr)
        report = json.loads((folder / 'android-result.json').read_text())
        assert report['exit_code'] == code and report['android_execution'] is False
        attempts = (folder / 'attempts.txt').read_text().splitlines();assert len(attempts) == names and len(set(attempts)) == names
        assert report['tests'][0]['signal'] == expected_signal
        assert report['tests'][0]['deadline_exceeded'] == (mode == 'timeout')
print('PASS host collector and real supervisor fork/exit/signal6/deadline14/missing-report retention; zero Android executions')
