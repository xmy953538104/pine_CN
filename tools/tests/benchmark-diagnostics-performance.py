#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Controlled host comparisons, without timing pass thresholds or Android performance claims."""
import argparse,hashlib,json,platform,statistics,subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=args.output.resolve()
root.mkdir(parents=True,exist_ok=False)
repo=Path(__file__).resolve().parents[2]
fixtures=repo/'tests/diagnostics/performance'
rows=[]
commands=[]
for optimization in ['0','2']:
 common=['c++','-std=c++20','-O'+optimization,'-g','-fno-omit-frame-pointer','-pthread','-Itests/diagnostics/storage_stubs','-Iapp/src/main/cpp/skyline']
 sink=root/('sink-O'+optimization)
 storage=root/('storage-O'+optimization)
 for cmd in [common+['app/src/main/cpp/skyline/diagnostics/diagnostics.cpp',str(fixtures/'sink-probe.cpp'),'-o',str(sink)],
             common+['-Itests/diagnostics/storage_stubs','app/src/main/cpp/skyline/diagnostics/diagnostics.cpp','app/src/main/cpp/skyline/services/fssrv/IStorage.cpp','tests/diagnostics/storage_benchmark.cpp','-o',str(storage)]]:
  commands.append(cmd);subprocess.run(cmd,cwd=repo,check=True)
 for trial in range(3):
  modes=['off','normal','detailed'];modes=modes[trial:]+modes[:trial]
  for mode in modes:
   dest=root/f'storage-O{optimization}-{trial}-{mode}'
   result=json.loads(subprocess.check_output([str(storage),mode,'1000000',str(dest),str(trial)],cwd=repo,text=True))
   assert result['checksum']==39500000 and result['backing_calls']==1000000 and result['accepted']==result['written']
   result.update(optimization=optimization,fixture='production-IStorage',trial=trial,ns_per_read=result['calls_ns']/1000000)
   rows.append(result)
 for kind,threads,count,pace in [('ordinary',1,20000,0),('ordinary',4,20000,0),('critical',4,20000,0),('guest',1,20000,0),('legacy',4,20000,0),('critical',1,24,250000)]:
  for mode in ['off','detailed']:
   dest=root/f'sink-O{optimization}-{kind}-{threads}-{pace}-{mode}'
   result=json.loads(subprocess.check_output([str(sink),kind,mode,str(dest),str(threads),str(count),str(pace)],cwd=repo,text=True))
   result.update(optimization=optimization,fixture='production-sink')
   rows.append(result)
 result={'scope':'HOST ONLY; unchanged production991 sink/IStorage, controlled inputs. Not physical ARM64/game FPS/logd/Android flash/ART/thermal evidence.',
         'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
         'compiler':subprocess.check_output(['c++','--version'],text=True).splitlines()[0],'host':platform.platform(),
         'commands':commands,'rows':rows}
 (root/'probe-results.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps({'optimization':optimization,'storage_medians_ns':{mode:statistics.median(r['ns_per_read'] for r in rows if r.get('fixture')=='production-IStorage' and r['optimization']==optimization and r['mode']==mode) for mode in ['off','normal','detailed']}}),flush=True)

# Use the exact production AudioCore adapters; host stand-ins affect only logd/locks.
audio=(repo/'app/src/main/cpp/skyline/audio.cpp').read_text()
block=audio[audio.index('namespace AudioCore::Log {'):audio.index('namespace Core::Timing {')]
adapter=root/'audio-log-adapter.cpp'
adapter.write_text('#include "logger/logger.h"\n#include "diagnostics/legacy_error.h"\n'+block)
legacy=root/'legacy-probe'
command=['c++','-std=c++20','-O2','-g','-pthread','-DFMT_HEADER_ONLY',
         '-I'+str(fixtures/'legacy-stubs'),'-Iapp/src/main/cpp/skyline','-Iapp/libraries/fmt/include',
         'app/src/main/cpp/skyline/logger/logger.cpp','app/src/main/cpp/skyline/diagnostics/diagnostics.cpp',
         str(adapter),str(fixtures/'legacy-probe.cpp'),'-o',str(legacy)]
subprocess.run(command,cwd=repo,check=True)
legacy_rows=[]
for path in ['guarded','adapter']:
 for mode in ['off','on']:
  completed=subprocess.run([str(legacy),path,mode,str(root/f'legacy-{path}-{mode}')],
                           cwd=repo,text=True,capture_output=True,timeout=15,check=True)
  row=json.loads(completed.stdout)
  assert row['unfiltered_legacy_writes']==(4096 if path=='adapter' else 0)
  legacy_rows.append(row)
(root/'legacy-results.json').write_text(json.dumps(legacy_rows,indent=2)+'\n')
print(json.dumps({'legacy':legacy_rows}),flush=True)
