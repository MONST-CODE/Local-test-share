#!/usr/bin/env python3
"""Build deterministic minimal PE32+ resource fixtures for the C++ resource probe."""
from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass, field
from pathlib import Path

FILE_ALIGNMENT = 0x200
SECTION_RVA = 0x1000
SECTION_RAW = 0x200


@dataclass
class Node:
    children: dict[int, "Node"] = field(default_factory=dict)
    data: bytes | None = None
    directory_offset: int = 0
    data_entry_offset: int = 0
    payload_offset: int = 0


def utf16z(text: str) -> bytes:
    return text.encode("utf-16le") + b"\0\0"


def version_resource(version: tuple[int, int, int, int]) -> bytes:
    key = utf16z("VS_VERSION_INFO")
    value_offset = (6 + len(key) + 3) & ~3
    file_ms = (version[0] << 16) | version[1]
    file_ls = (version[2] << 16) | version[3]
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x00010000, file_ms, file_ls,
                        file_ms, file_ls, 0x3F, 0, 0x00040004, 1, 0, 0, 0)
    length = value_offset + len(fixed)
    return struct.pack("<HHH", length, len(fixed), 0) + key + bytes(value_offset - 6 - len(key)) + fixed


def group_icon(count: int) -> bytes:
    sizes_and_bytes = [(16, 40), (24, 48), (32, 56), (48, 64),
                       (64, 72), (128, 80), (256, 96)]
    entries = bytearray()
    for index, (size, image_bytes) in enumerate(sizes_and_bytes[:count], 1):
        entries += struct.pack("<BBBBHHIH", 0 if size == 256 else size,
                               0 if size == 256 else size, 0, 0, 1, 32, image_bytes, index)
    return struct.pack("<HHH", 0, 1, count) + entries


def make_resource_tree(with_manifest: bool, group_count: int, manifest_version: str) -> Node:
    root = Node()
    icon_children = {i: Node(children={1033: Node(data=bytes([0x28, 0, 0, 0]) + bytes([i]) * max(0, n - 4))})
                     for i, n in enumerate([40, 48, 56, 64, 72, 80, 96], 1)}
    root.children[3] = Node(children=icon_children)  # RT_ICON
    root.children[14] = Node(children={1: Node(children={1033: Node(data=group_icon(group_count))})})
    root.children[16] = Node(children={1: Node(children={1033: Node(data=version_resource((0, 2, 0, 0)))})})
    if with_manifest:
        xml = (f'<?xml version="1.0" encoding="UTF-8"?><assembly><assemblyIdentity '
               f'version="{manifest_version}" name="DevLink.Desktop"/><trustInfo>'
               f'<requestedExecutionLevel level="asInvoker" uiAccess="false"/></trustInfo>'
               f'<application><windowsSettings><dpiAwareness>PerMonitorV2</dpiAwareness>'
               f'</windowsSettings></application></assembly>').encode("utf-8")
        root.children[24] = Node(children={1: Node(children={1033: Node(data=xml)})})
    return root


def directories_preorder(node: Node, output: list[Node]) -> None:
    output.append(node)
    for child in node.children.values():
        if child.data is None:
            directories_preorder(child, output)


def leaves_preorder(node: Node, output: list[Node]) -> None:
    for child in node.children.values():
        if child.data is None:
            leaves_preorder(child, output)
        else:
            output.append(child)


def build_resource_blob(root: Node) -> bytes:
    directories: list[Node] = []
    leaves: list[Node] = []
    directories_preorder(root, directories)
    leaves_preorder(root, leaves)
    cursor = 0
    for node in directories:
        node.directory_offset = cursor
        cursor += 16 + len(node.children) * 8
    for node in leaves:
        cursor = (cursor + 3) & ~3
        node.data_entry_offset = cursor
        cursor += 16
    for node in leaves:
        cursor = (cursor + 3) & ~3
        node.payload_offset = cursor
        cursor += len(node.data or b"")

    blob = bytearray(cursor)
    for node in directories:
        start = node.directory_offset
        struct.pack_into("<IIHHHH", blob, start, 0, 0, 0, 0, 0, len(node.children))
        position = start + 16
        for resource_id, child in sorted(node.children.items()):
            struct.pack_into("<II", blob, position, resource_id,
                             (0x80000000 | child.directory_offset) if child.data is None else child.data_entry_offset)
            position += 8
    for node in leaves:
        payload = node.data or b""
        struct.pack_into("<IIII", blob, node.data_entry_offset,
                         SECTION_RVA + node.payload_offset, len(payload), 0, 0)
        blob[node.payload_offset:node.payload_offset + len(payload)] = payload
    return bytes(blob)


def make_pe(resource_blob: bytes) -> bytes:
    raw_size = (len(resource_blob) + FILE_ALIGNMENT - 1) & ~(FILE_ALIGNMENT - 1)
    headers = bytearray(SECTION_RAW)
    headers[0:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, 0x80)
    pe = 0x80
    headers[pe:pe + 4] = b"PE\0\0"
    coff = pe + 4
    struct.pack_into("<HHIIIHH", headers, coff,
                     0x8664, 1, 0, 0, 0, 240, 0x0022)
    optional = coff + 20
    struct.pack_into("<HBB", headers, optional, 0x20B, 14, 0)
    struct.pack_into("<III", headers, optional + 4, raw_size, 0, 0)
    struct.pack_into("<II", headers, optional + 16, SECTION_RVA, 0x1000)
    struct.pack_into("<I", headers, optional + 32, 0x1000)
    struct.pack_into("<I", headers, optional + 36, FILE_ALIGNMENT)
    struct.pack_into("<HHHHHH", headers, optional + 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into("<I", headers, optional + 56, 0x2000)
    struct.pack_into("<I", headers, optional + 60, SECTION_RAW)
    struct.pack_into("<H", headers, optional + 68, 3)
    struct.pack_into("<I", headers, optional + 72, 0x100000)
    struct.pack_into("<I", headers, optional + 76, 0x1000)
    struct.pack_into("<I", headers, optional + 80, 0x100000)
    struct.pack_into("<I", headers, optional + 84, 0x1000)
    struct.pack_into("<I", headers, optional + 108, 16)
    struct.pack_into("<II", headers, optional + 112 + 2 * 8, SECTION_RVA, len(resource_blob))
    section = optional + 240
    headers[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", headers, section + 8, len(resource_blob), SECTION_RVA,
                     raw_size, SECTION_RAW)
    struct.pack_into("<I", headers, section + 36, 0x40000040)
    return bytes(headers) + resource_blob + bytes(raw_size - len(resource_blob))


def write_fixture(path: Path, *, manifest: bool = True, group_count: int = 7,
                  manifest_version: str = "0.2.0.0") -> None:
    blob = build_resource_blob(make_resource_tree(manifest, group_count, manifest_version))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(make_pe(blob))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    write_fixture(args.out_dir / "icons-good.exe")
    write_fixture(args.out_dir / "icons-bad-group.exe", group_count=6)
    write_fixture(args.out_dir / "no-manifest.exe", manifest=False)
    write_fixture(args.out_dir / "wrong-version.exe", manifest_version="9.9.9.9")
    print(f"Wrote four synthetic PE32+ x64 parser fixtures; ICO entries=7; output={args.out_dir}")


if __name__ == "__main__":
    main()
