#!/usr/bin/env python3
"""Only host-side planner checks here. Executed ARM64 has its own required CI."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='pine-entry-host-') as directory:
    subprocess.run([sys.executable, str(root/'tools/run-abort-entry-prototype.py'),
                    '--host-only', '--output', directory], check=True, timeout=90)
