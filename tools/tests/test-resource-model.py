#!/usr/bin/env python3
"""The fast tools/test-*.py glob runs the isolated, content-free C++ model."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

if __name__ == '__main__':
    subprocess.run(['bash', str(ROOT / 'tools/tests/run-resource-model.sh')], cwd=ROOT, check=True, timeout=90)
