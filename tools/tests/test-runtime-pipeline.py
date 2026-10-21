#!/usr/bin/env python3
"""Extract current production bodies; controlled dependencies, never game evidence."""
import argparse, hashlib, json, os, shutil, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CPP=ROOT/'app/src/main/cpp/skyline'
def extract(path,start,end):
    text=(CPP/path).read_text();return text[text.index(start):text.index(end,text.index(start))]
def main():
    p=argparse.ArgumentParser();p.add_argument('--aarch64',action='store_true');p.add_argument('--output',type=Path);p.add_argument('--compile-only',action='store_true');p.add_argument('--cxx');a=p.parse_args()
    out=a.output or Path(tempfile.mkdtemp(prefix='pine-runtime-pipeline-'));out.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'tests/runtime_observer/pipeline.cpp.in').read_text()
    bodies={
      'ACTUAL_NCE_BODIES':extract(Path('nce.cpp'),'    constexpr size_t TrampolineSize','    size_t NCE::GetHookSectionSize'),
      'ACTUAL_LOAD_EXECUTABLE':extract(Path('loader/loader.cpp'),'    Loader::ExecutableLoadInfo Loader::LoadExecutable','    template<ElfSymbol ElfSym>'),
      'ACTUAL_NSO_HEADER':extract(Path('loader/nso.h'),'        union NsoFlags','        /**'),
      'ACTUAL_LOAD_NSO':extract(Path('loader/nso.cpp'),'    Loader::ExecutableLoadInfo NsoLoader::LoadNso','    void *NsoLoader::LoadProcessData'),
      'ACTUAL_NCE_INIT':extract(Path('kernel/types/KThread.cpp'),'    void KNceThread::Init()','    void KNceThread::Run()'),
      'ACTUAL_NCE_RUN':extract(Path('kernel/types/KThread.cpp'),'    void KNceThread::Run()','    void KJit32Thread::Init()')}
    for name,body in bodies.items():source=source.replace('/* '+name+' */',body)
    generated=out/'pipeline.cpp';generated.write_text(source)
    cxx=a.cxx or ('aarch64-linux-gnu-g++' if a.aarch64 else os.environ.get('CXX','c++'))
    assembly=CPP/'nce/guest.S' if a.aarch64 else out/'host-stubs.S'
    if not a.aarch64:assembly.write_text('.text\n.global SaveCtx\nSaveCtx:\n.fill 38,4,0xd503201f\n.global LoadCtx\nLoadCtx:\n.fill 36,4,0xd503201f\n.section .note.GNU-stack,"",@progbits\n')
    command=[cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-reorder','-pthread','-DPINE_DIAG_RUNTIME_TEST','-I'+str(ROOT/'tests/runtime_observer/stubs'),'-I'+str(CPP),'-I'+str(CPP/'diagnostics'),str(generated),str(CPP/'diagnostics/runtime_observer.cpp'),str(CPP/'diagnostics/nso_image.cpp'),str(CPP/'diagnostics/diagnostics.cpp'),str(CPP/'nce/observation.cpp'),str(assembly)]
    if a.aarch64:command.append(str(ROOT/'tests/runtime_observer/runtime_fixture.S'))
    if 'android' in cxx:command.append('-llog')
    command+=['-o',str(out/'pipeline')]
    subprocess.run(command,check=True)
    run=['qemu-aarch64','-L','/usr/aarch64-linux-gnu'] if a.aarch64 else []
    if not a.compile_only:subprocess.run([*run,str(out/'pipeline'),str(out/'logs')],check=True,timeout=90)
    result={'source_sha':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'source_body_sha256':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in bodies.items()},'execution':'compile only, not executed' if a.compile_only else 'QEMU Linux-user AArch64 on x86_64' if a.aarch64 else 'host mapping only','runtime_connected':False,'android_lifecycle':False,'original_dependencies':'controlled VFS/mapping/HLE; actual extracted Loader/NCE/KNce Init+Run bodies'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
