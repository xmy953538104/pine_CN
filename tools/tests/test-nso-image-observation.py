#!/usr/bin/env python3
"""Compile real owned metadata and current LoadNso body with controlled loader inputs."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIAG = ROOT/'app/src/main/cpp/skyline/diagnostics'


def main():
    with tempfile.TemporaryDirectory(prefix='pine-nso-identity-') as temporary:
        out = Path(temporary)
        common = [os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I'+str(DIAG)]
        sources = [str(DIAG/'nso_image.cpp'), str(DIAG/'diagnostics.cpp'), str(DIAG/'runtime_observer.cpp'), str(DIAG.parent/'nce/observation.cpp')]
        subprocess.run([*common, str(ROOT/'tests/abort_entry/nso_image_test.cpp'), *sources, '-o', str(out/'metadata')], check=True)
        subprocess.run([str(out/'metadata'), str(out/'logs')], check=True)
        header = (ROOT/'app/src/main/cpp/skyline/loader/nso.h').read_text()
        structs = header[header.index('        union NsoFlags'):header.index('        /**', header.index('static_assert(sizeof(NsoHeader)'))]
        source = (ROOT/'app/src/main/cpp/skyline/loader/nso.cpp').read_text()
        body = source[source.index('    Loader::ExecutableLoadInfo NsoLoader::LoadNso'):source.index('    void *NsoLoader::LoadProcessData')]
        template = (ROOT/'tests/abort_entry/nso_loader_harness.cpp.in').read_text()
        generated = out/'load-nso.cpp'
        generated.write_text(template.replace('/* REAL_HEADER_STRUCTS */', structs).replace('/* REAL_LOAD_NSO */', body))
        subprocess.run([*common, '-Wno-unused-parameter', str(generated), *sources, '-o', str(out/'loader')], check=True)
        subprocess.run([str(out/'loader'), str(out/'loader-logs')], check=True)


if __name__ == '__main__':
    main()
