#!/usr/bin/env python3
"""Create a fresh, tiny corpus. Refuse to overwrite an existing directory."""
from pathlib import Path
import struct
import sys
root = Path(sys.argv[1] if len(sys.argv) > 1 else 'out/fuzz/fresh-corpus')
root.mkdir(parents=True, exist_ok=False)
fmt = '>IHHHHQQIQQII'
root.joinpath('chat').write_bytes(struct.pack(fmt, 0x504c5054, 1, 0, 8, 0, 1, 1, 4, 1, 0, 2, 0) + b'ok')
root.joinpath('hello').write_bytes(struct.pack(fmt, 0x504c5054, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0))
print('Created two corpus entries in', root)
