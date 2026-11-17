#!/usr/bin/env python3
"""DevLink — fabrique de PE synthétiques minimaux pour valider la sonde.

Outil de MAINTENANCE (hors barrières de build) : il construit des images PE32+
avec une vraie table de ressources (.rsrc : RT_ICON, RT_GROUP_ICON, RT_VERSION,
RT_MANIFEST) afin de prouver que tests/build/toolchain_probe.cpp inspecte
réellement les PE — y compris ses chemins de refus — depuis Linux, où aucun
linker Windows n'est disponible. Les .exe produits sont des artefacts de test,
jamais livrés.

Usage (vérification de la sonde, positive et négative) :
    python3 tests/build/synthetic_pe.py /tmp/good.exe
    python3 tests/build/synthetic_pe.py /tmp/badcount.exe --group-declares 3
    python3 tests/build/synthetic_pe.py /tmp/noversion.exe --drop-version
    python3 tests/build/synthetic_pe.py /tmp/v0300.exe --version 0.3.0.0
    probe --pe-icons /tmp/good.exe --expect-icons 2        # RC=0
    probe --pe-icons /tmp/badcount.exe --expect-icons 2    # RC=1 groupe incohérent
    probe --version-info /tmp/good.exe --expect-version 0.2.0.0   # RC=0
    probe --version-info /tmp/v0300.exe --expect-version 0.2.0.0  # RC=1 divergence
"""
import argparse
import struct


def build_pe(path, icon_count=2, group_declares=None, version=(0, 2, 0, 0),
             drop_version=False, manifest=True):
    if group_declares is None:
        group_declares = icon_count
    icon_payloads = [b'\x28\x00\x00\x00' + bytes(60) for _ in range(icon_count)]
    grp = struct.pack('<HHH', 0, 1, group_declares)
    for i in range(group_declares):
        size = len(icon_payloads[i]) if i < icon_count else 64
        grp += struct.pack('<BBBBHHIH', 16, 16, 0, 0, 1, 32, size, i + 1)
    key = 'VS_VERSION_INFO'.encode('utf-16-le') + b'\x00\x00'
    fixed = struct.pack('<II', 0xFEEF04BD, 0x00010000)
    fixed += struct.pack('<IIII', version[0] << 16 | version[1],
                         version[2] << 16 | version[3],
                         version[0] << 16 | version[1],
                         version[2] << 16 | version[3])
    fixed += bytes(52 - len(fixed))
    ver_payload = struct.pack('<HHH', 6 + 32 + 2 + 52, 52, 0) + key + b'\x00\x00' + fixed
    man_payload = b'<?xml version="1.0"?><assembly><trustInfo/></assembly>'

    tree = {3: [(i + 1, icon_payloads[i]) for i in range(icon_count)],
            14: [(2, grp)]}
    if not drop_version:
        tree[16] = [(1, ver_payload)]
    if manifest:
        tree[24] = [(1, man_payload)]
    types = sorted(tree)
    counts = {t: len(tree[t]) for t in types}

    def dirpack(buf, off, named, ids):
        struct.pack_into('<IIHHHH', buf, off, 0, 0, 0, 0, named, ids)

    off_dir2 = 16 + 8 * len(types)
    off_dir3 = off_dir2 + sum(16 + 8 * counts[t] for t in types)
    off_leaf = off_dir3 + sum(24 for t in types for _ in range(counts[t]))
    cur = off_leaf + 16 * sum(counts[t] for t in types)
    pay_pos = {}
    for t in types:
        for (nid, pay) in tree[t]:
            pay_pos[(t, nid)] = cur
            cur = (cur + len(pay) + 3) & ~3
    buf = bytearray(cur)
    dirpack(buf, 0, 0, len(types))
    e, d2, d3, lf = 16, off_dir2, off_dir3, off_leaf
    for t in types:
        struct.pack_into('<II', buf, e, t, 0x80000000 | d2)
        e += 8
        dirpack(buf, d2, 0, counts[t])
        e2 = d2 + 16
        for (nid, pay) in tree[t]:
            struct.pack_into('<II', buf, e2, nid, 0x80000000 | d3)
            e2 += 8
            dirpack(buf, d3, 0, 1)
            struct.pack_into('<II', buf, d3 + 16, 0x040C, lf)
            struct.pack_into('<IIII', buf, lf, 0x1000 + pay_pos[(t, nid)], len(pay), 0, 0)
            d3 += 24
            lf += 16
            buf[pay_pos[(t, nid)]:pay_pos[(t, nid)] + len(pay)] = pay
        d2 += 16 + 8 * counts[t]

    dos = bytearray(64)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 64)
    coff = struct.pack('<HHIIIHH', 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = bytearray(240)
    struct.pack_into('<H', opt, 0, 0x20B)
    struct.pack_into('<II', opt, 112 + 16, 0x1000, len(buf))
    sec = bytearray(40)
    sec[0:6] = b'.rsrc\x00'
    struct.pack_into('<I', sec, 8, len(buf))
    struct.pack_into('<I', sec, 12, 0x1000)
    struct.pack_into('<I', sec, 16, len(buf))
    struct.pack_into('<I', sec, 20, 0x200)
    struct.pack_into('<I', sec, 36, 0x40000040)
    image = bytearray(dos + b'PE\x00\x00' + coff + opt + sec)
    image += bytes(0x200 - len(image))
    image += buf
    with open(path, 'wb') as handle:
        handle.write(bytes(image))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('output')
    parser.add_argument('--icons', type=int, default=2)
    parser.add_argument('--group-declares', type=int, default=None,
                        help='force un compte GRPICONDIR incohérent (test négatif)')
    parser.add_argument('--version', default='0.2.0.0')
    parser.add_argument('--drop-version', action='store_true')
    parser.add_argument('--drop-manifest', action='store_true')
    args = parser.parse_args()
    quad = tuple(int(part) for part in args.version.split('.'))
    if len(quad) != 4:
        parser.error('--version attend A.B.C.D')
    build_pe(args.output, icon_count=args.icons, group_declares=args.group_declares,
             version=quad, drop_version=args.drop_version, manifest=not args.drop_manifest)
    print(f'PE synthetique ecrit : {args.output}')


if __name__ == '__main__':
    main()
