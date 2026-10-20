#!/usr/bin/env python3
"""Actual native sink/resource APIs and a frozen guest-SVC behavior guard."""
import hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
SVC=ROOT/'app/src/main/cpp/skyline/kernel/svc.cpp'
EXPECTED={'SetHeapSize':'11f0029409af907b6c897aa30ade82223a877865ac2723a2a0d481af5ea2dbb6','GetInfo':'bd11abc8caf4c289b8f665afc7901e0b23e0a2da46fe1cd2baec18f83e2db8e4'}
# These are token hashes of the reviewed aabd88a4 implementation, not a generated
# expected result from the implementation under test.
def function(s,name,end): return s[s.index('    void '+name+'('):s.index(end,s.index('    void '+name+'('))]
def digest(s):
    s=re.sub(r'//[^\n]*|/\*.*?\*/','',s,flags=re.S)
    return hashlib.sha256(''.join(re.findall(r'"(?:\\.|[^"\\])*"|[A-Za-z0-9_]+|[^\s]',s)).encode()).hexdigest()
s=SVC.read_text()
for name,end in [('SetHeapSize','    void SetMemoryPermission('),('GetInfo','    void MapPhysicalMemory(')]:
    body=function(s,name,end)
    if name=='GetInfo':body=body[:body.index('        // Reuse values already computed')]+'    }\n'
    else:body=re.sub(r'^\s*diagnostics::Resource(?:Add|Set)\([^;]*\);\n','',body,flags=re.M)
    assert digest(body)==EXPECTED[name],name+' changed guest behavior beyond reviewed observation calls'
    if name=='GetInfo':assert digest(body.replace('0xF8000000','0x200000000'))!=EXPECTED[name]
memory=(ROOT/'app/src/main/cpp/skyline/kernel/memory.cpp').read_text()
as39=memory.split('namespace AS39bit {',1)[1].split('\n        }',1)[0]
assert 'constexpr size_t HeapRegionSize{0x180000000}' in as39
process=(ROOT/'app/src/main/cpp/skyline/kernel/types/KProcess.cpp').read_text()
assert 'tid{threads.size() + 1}' in process and 'threads.push_back(thread)' in process
nce=(ROOT/'app/src/main/cpp/skyline/nce.cpp').read_text()
assert 'ResourceAdd(diagnostics::SvcEnteredMetric(state.thread->id))' in nce
assert 'ResourceAdd(diagnostics::SvcReturnedMetric(state.thread->id))' in nce
print('PASS actual HOS-1 first-thread source bound to new main/other SVC classification')
print('PASS unchanged GetInfo/SetHeapSize tokens after exact observer removal; guest cap 0xF8000000; virtual region remains 6 GiB')
flags=['-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread']
if os.environ.get('PINE_DIAG_SANITIZE')=='1':flags+=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
with tempfile.TemporaryDirectory(prefix='pine-resource-observer-') as temp:
    directory=Path(temp); exe=directory/'observer'
    core=ROOT/'app/src/main/cpp/skyline/diagnostics'
    subprocess.run([os.environ.get('CXX','c++'),*flags,'-I'+str(core),str(core/'diagnostics.cpp'),str(ROOT/'tests/diagnostics/resource_observer_test.cpp'),'-o',str(exe)],check=True,timeout=90)
    subprocess.run([str(exe),str(directory/'logs')],check=True,timeout=30)
    rows=[json.loads(line) for p in (directory/'logs').glob('diagnostics-*.jsonl') for line in p.read_text().splitlines()]
    host=[r for r in rows if r['event']=='HOST_MEMORY_SAMPLE']; assert len(host)==1
    assert host[0]['session_id']=='resource-session' and host[0]['launch_id']=='resource-launch'
    samples=[r for r in rows if r['event']=='RESOURCE_GPU_SAMPLE']
    assert any(r['fields']['host_submit_accepted']=='1000000' for r in samples),'actual counter update missing'
    caps=[r for r in rows if r['event']=='RESOURCE_CAPABILITIES']; assert caps
    assert all(r['fields']['nvn_command_capacity'].startswith('unavailable') and r['fields']['guest_program_build_id']=='not_observed' for r in caps)
    # Maximum public identities and sampled values must still fit the Logcat path.
    assert all(len(json.dumps(r,ensure_ascii=False,separators=(',',':')).encode())<=3800 for r in rows if r['category']=='resource')
    window_path=directory/'logs/resource-window.jsonl'
    data=window_path.read_bytes();assert len(data)<=131072
    window=[json.loads(line) for line in data.splitlines()]
    assert window[0]['record_kind']=='resource_window_metadata' and window[0]['reason']=='session_end'
    assert window[0]['time_scope']=='writer_ingestion_monotonic_not_event_execution'
    assert window[0]['session_id']=='resource-session' and window[0]['retained_records']==len(window)-1
    assert any(r['event']=='RESOURCE_GPU_SAMPLE' and r['fields']['host_submit_accepted']=='1000000' for r in window[1:])
    failure=directory/'failure'
    subprocess.run([os.environ.get('CXX','c++'),*flags,'-DPINE_DIAG_TEST_HOOKS','-I'+str(core),str(core/'diagnostics.cpp'),str(ROOT/'tests/diagnostics/resource_failure_test.cpp'),'-o',str(failure)],check=True,timeout=90)
    subprocess.run([str(failure),str(directory/'failure-logs')],check=True,timeout=10)
    failure_rows=[json.loads(line) for p in (directory/'failure-logs').glob('diagnostics-*.jsonl') for line in p.read_text().splitlines()]
    assert any(r['event']=='AFTER_RESOURCE_ALLOCATION_FAILURE' for r in failure_rows)
print('PASS actual native resource counters, default-off/zero producer allocation, exact owner admission and bounded ADB events')
