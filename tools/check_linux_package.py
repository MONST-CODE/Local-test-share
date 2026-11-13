#!/usr/bin/env python3
"""Verify a locally built Release CLI TGZ. Never run against untrusted packages.
The extracted executable must match the caller's reference binary before execution.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('reference_binary', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root / 'out'
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='package-verify-', dir=output) as directory:
        with tarfile.open(args.archive) as archive:
            members = archive.getmembers()
            for member in members:
                path = PurePosixPath(member.name)
                require(not path.is_absolute() and '..' not in path.parts
                        and (member.isdir() or member.isfile()), f'Unsafe member: {member.name}')
            # Every member was checked above; no links or special files permitted.
            archive.extractall(directory)
        files = [p for p in Path(directory).rglob('*') if p.is_file()]
        programs = [p for p in files if p.name == 'plpt-monitor']
        require(len(programs) == 1, 'Expected exactly one monitor')
        for prohibited in ['plpt-unit-tests', 'plpt-platform-tests', 'plpt-fuzz-envelope', 'plpt-desktop', 'libplpt_models.a']:
            require(not any(p.name == prohibited for p in files), f'Unwanted artifact: {prohibited}')
        for required in ['README.md', 'BUILD.md', 'SECURITY.md', 'TESTING.md', 'VALIDATION.md', 'QUICKSTART.md', 'DEPENDENCIES.md']:
            require(any(p.name == required for p in files), f'Missing document: {required}')
        print('PASS: TGZ paths contained; only the requested Release CLI is packaged; docs present.')
        program = programs[0]
        require(program.read_bytes() == args.reference_binary.read_bytes(), 'Binary differs from tested reference')
        print('PASS: extracted binary matches the tested Release binary exactly.')
        result = subprocess.run([str(program), '--version'], capture_output=True, text=True, check=True)
        require('no sharing' in result.stdout, 'Wrong version/scope output')
        print('Extracted binary:', result.stdout.strip())
        live = subprocess.run([str(program)], capture_output=True, text=True, check=True)
        print('Live inventory:', live.stdout.splitlines()[-1])
        symbols = subprocess.run(['nm', '-C', str(program)], capture_output=True, text=True, check=True)
        require('plpt::model::' not in symbols.stdout, 'Experimental models present in monitor')
        print('PASS: no plpt::model:: symbols in the monitor binary.')
        print('SHA256', hashlib.sha256(args.archive.read_bytes()).hexdigest(), args.archive)
        print('Archive bytes:', args.archive.stat().st_size)
        print('Dependencies (local Linux binary, not a universal Linux release):')
        print(subprocess.run(['ldd', str(program)], capture_output=True, text=True, check=True).stdout)


if __name__ == '__main__':
    main()
