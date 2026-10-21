#!/usr/bin/env python3
"""Production identity wiring with controlled host data; no Guest/device claim."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
CORE=ROOT/'app/src/main/cpp/skyline/diagnostics'
with tempfile.TemporaryDirectory(prefix='pine-production-gate-') as directory:
    output=Path(directory)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-I'+str(CORE),
                    str(ROOT/'tests/runtime_observer/production_gate_test.cpp'),str(CORE/'runtime_observer.cpp'),str(CORE.parent/'nce/observation.cpp'),str(CORE/'nso_image.cpp'),str(CORE/'diagnostics.cpp'),'-o',str(output/'gate')],check=True)
    subprocess.run([str(output/'gate'),str(output/'logs')],check=True)

with tempfile.TemporaryDirectory(prefix='pine-runtime-observer-') as directory:
    output=Path(directory)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-DPINE_DIAG_RUNTIME_TEST','-I'+str(CORE),
                    str(ROOT/'tests/runtime_observer/identity_test.cpp'),str(CORE/'runtime_observer.cpp'),str(CORE.parent/'nce/observation.cpp'),str(CORE/'nso_image.cpp'),str(CORE/'diagnostics.cpp'),'-o',str(output/'identity')],check=True)
    subprocess.run([str(output/'identity'),str(output/'logs')],check=True)

with tempfile.TemporaryDirectory(prefix='pine-runtime-emitter-') as directory:
    output=Path(directory)/'emitter'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(CORE.parent/'nce'),str(ROOT/'tests/runtime_observer/patch_test.cpp'),str(CORE.parent/'nce/observation.cpp'),'-o',str(output)],check=True)
    subprocess.run([str(output)],check=True)
