#!/usr/bin/env python3
"""Check that each applicable engine header is self-contained (native compiler)."""
from pathlib import Path
import os
import subprocess
import sys
root = Path(__file__).resolve().parent.parent
compiler = os.environ.get('CXX', 'clang++')
count = 0
for header in sorted((root / 'engine').rglob('*.hpp')):
    if ('windows' in header.parts and sys.platform != 'win32') or ('linux' in header.parts and sys.platform == 'win32'):
        continue
    flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only', '-x', 'c++', '-I', str(root)]
    if sys.platform == 'win32':
        flags += ['-D_WIN32_WINNT=0x0A00', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN']
    relative = header.relative_to(root).as_posix()
    print('HEADER', relative, flush=True)
    subprocess.run(flags + ['-'], input=f'#include "{relative}"\n', text=True, cwd=root, check=True)
    count += 1
print(f'PASS: {count} independent headers', flush=True)
