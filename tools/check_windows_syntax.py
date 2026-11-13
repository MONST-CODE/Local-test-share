#!/usr/bin/env python3
"""Additional Windows x64 syntax check, NOT Windows execution or MSVC ABI validation.
Use clang++ and Zig 0.14.1's LLVM/MinGW headers; never ship these sysroot artifacts.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--clang', default='clang++')
p.add_argument('--zig-lib', required=True, type=Path)
p.add_argument('--sdl-root', type=Path)
p.add_argument('--imgui-root', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parent.parent
# libc++19 fourni par Zig place encore jthread/stop_token derrière ce drapeau.
# Ceci ne concerne PAS la STL Microsoft de la compilation Windows officielle.
flags = [a.clang, '--target=x86_64-w64-windows-gnu', '-std=c++20', '-fsyntax-only', '-nostdinc++', '-fexperimental-library',
         '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wshadow', '-I', str(root),
         '-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS', '-D_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS',
         '-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS', '-D_LIBCPP_PSTL_BACKEND_SERIAL',
         '-D_LIBCPP_ABI_VERSION=1', '-D_LIBCPP_ABI_NAMESPACE=__1',
         '-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG',
         '-D_WIN32_WINNT=0x0A00', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-DUNICODE', '-D_UNICODE']
for sub in ['libcxx/include', 'libcxxabi/include', 'libc/include/any-windows-any']:
    flags += ['-isystem', str(a.zig_lib / sub)]
files = sorted(f for f in (root / 'engine').rglob('*.cpp') if 'linux' not in f.parts)
files += [root / 'app/host_shell/main.cpp', root / 'tests/unit/test_main.cpp', root / 'tests/integration/platform_tests.cpp']
if a.sdl_root and a.imgui_root:
    files += [root / 'app/desktop_ui/main.cpp']
    for directory in [a.sdl_root / 'include', a.imgui_root, a.imgui_root / 'backends']:
        flags += ['-isystem', str(directory)]
print('Windows x64 syntax only / Clang + MinGW declarations / no execution', flush=True)
print(shlex.join(flags), flush=True)
for f in files:
    print('CHECK', f.relative_to(root), flush=True)
    subprocess.run(flags + [str(f)], cwd=root, check=True)
print(f'PASS: {len(files)} translation units; native Windows build and tests still required.', flush=True)
