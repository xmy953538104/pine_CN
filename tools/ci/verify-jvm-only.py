#!/usr/bin/env python3
"""Reject native/APK/signing tasks before the unit-test-only step runs."""
import sys
from pathlib import Path


def verify(text):
    tasks = [line.split()[0].rsplit(':', 1)[-1] for line in text.splitlines() if line.startswith(':')]
    forbidden = [task for task in tasks if task.startswith((
        'assemble', 'bundle', 'packageFull', 'validateSigning', 'buildCMake',
        'configureCMake', 'externalNativeBuild', 'stripDebugSymbols', 'mergeFullDebugNativeLibs'))]
    if 'testFullDebugUnitTest' not in tasks or forbidden:
        raise ValueError(f'Invalid JVM-only task graph: forbidden={forbidden}, tasks={tasks}')


if __name__ == '__main__':
    verify(Path(sys.argv[1]).read_text())
    print('Verified JVM-only graph: no native compilation, assembly, packaging or signing.')
