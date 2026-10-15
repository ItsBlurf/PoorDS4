# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect ELF load-page protection compatibility; no execution or console access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

PAGE = 0x4000


def inspect(path):
    data = path.read_bytes()
    errors = []
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected a complete little-endian ELF64 header')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    _, kind, machine, _, entry, offset, _, _, _, stride, count, _, _, _ = header
    if kind != 3 or machine != 62 or data[7] != 9:
        errors.append('Expected PS5 FreeBSD ELF64 DYN/x86-64')
    if stride != 56 or count > 128 or offset + stride * count > len(data):
        raise ValueError('Invalid or unbounded program header table')
    segments, permissions = [], {}
    reserved_end = 0
    protection_ends = []
    for index in range(count):
        fields = struct.unpack_from('<IIQQQQQQ', data, offset + index * stride)
        typ, flags, file_offset, address, _, file_size, memory_size, alignment = fields
        if typ != 1 or not memory_size:
            continue
        if memory_size > 0x4000000 or address > 0x4000000 or address + memory_size > 0x4000000:
            raise ValueError('Load region exceeds bounded payload inspection')
        if file_size > memory_size or file_offset + file_size > len(data):
            errors.append(f'LOAD {index}: invalid file/memory extent')
        if address % PAGE:
            errors.append(f'LOAD {index}: start {address:#x} is not 16 KiB aligned')
        if alignment != PAGE or (file_offset - address) % PAGE:
            errors.append(f'LOAD {index}: invalid PS5 page congruence/alignment')
        first = address // PAGE
        # Reference elfldr passes ROUND_PG(memsz), then FreeBSD's mprotect
        # also accounts for an unaligned address. This is a source-contract
        # model, not evidence of the console's running loader/kernel version.
        rounded_length = (memory_size + PAGE - 1) // PAGE * PAGE
        stop = (address + rounded_length + PAGE - 1) // PAGE
        reserved_end = max(reserved_end, (address + memory_size + PAGE - 1) // PAGE * PAGE)
        protection_ends.append((index, stop * PAGE))
        for page in range(first, stop):
            permissions[page] = flags
        segments.append({'index': index, 'address': address, 'file_size': file_size,
                         'memory_size': memory_size, 'flags': flags})
    for index, stop in protection_ends:
        if stop > reserved_end:
            errors.append(f'LOAD {index}: rounded protection extends past reserved payload mapping')
    executable = bool(permissions.get(entry // PAGE, 0) & 1)
    if not executable:
        errors.append('Entry page loses execute permission under reference loader protection order')
    if not any(s['address'] <= entry < s['address'] + s['file_size'] and s['flags'] & 1
               for s in segments):
        errors.append('Entry is not in file-backed executable bytes')
    return {'elf': str(path), 'sha256': hashlib.sha256(data).hexdigest(),
            'entry': entry, 'segments': segments,
            'entry_page_executable_after_reference_protections': executable,
            'passed': not errors, 'errors': errors, 'console_contacted': False,
            'scope': 'ELF and reference protection-order model; not native loader or gameplay proof'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    arguments = parser.parse_args()
    result = inspect(arguments.elf)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['passed'] else 1)
