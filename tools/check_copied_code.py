# SPDX-License-Identifier: GPL-3.0-or-later
"""Check final ELF copied reader or game ranges for external code/data use."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def inspect_instructions(disassembly, start, end):
    instructions, indirect_calls, failures = 0, 0, []
    for line in disassembly.splitlines():
        match = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if not match:
            continue
        address, mnemonic, operands = int(match[1], 16), match[2], match[3]
        if not start <= address < end:
            failures.append('instruction outside copied range: ' + line.strip())
        instructions += 1
        if '%rip' in operands:
            failures.append('RIP-relative reference: ' + line.strip())
        if mnemonic.startswith(('call', 'j', 'loop')):
            if operands.startswith('*'):
                if mnemonic.startswith('call'):
                    indirect_calls += 1
                continue
            target = re.match(r'(?:0x)?([0-9a-f]+)(?:\s|$)', operands)
            if not target or not start <= int(target[1], 16) < end:
                failures.append('external or unparsed branch: ' + line.strip())
    if not instructions or not indirect_calls:
        failures.append('missing decoded instructions or resolved indirect calls')
    return instructions, indirect_calls, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--kind', choices=('reader','game'), default='reader')
    args = parser.parse_args()
    symbols = subprocess.run(['llvm-nm', '-n', '--defined-only', str(args.elf)],
                             capture_output=True, text=True, check=True).stdout
    addresses = {}
    names = ('remote_pad_reader_stub','remote_pad_reader_stub_end') if args.kind == 'reader' else (
        'game_pad_read_state_stub','game_pad_bridge_stub_end')
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in names:
            addresses[fields[2]] = int(fields[0], 16)
    start, end = addresses[names[0]], addresses[names[1]]
    if not start < end or end - start > 65536:
        raise ValueError('invalid or unexpectedly large copied code range')
    disassembly = subprocess.run([
        'llvm-objdump', '--no-show-raw-insn', '--disassemble',
        '--start-address=' + hex(start), '--stop-address=' + hex(end), str(args.elf)],
        capture_output=True, text=True, check=True).stdout
    instructions, calls, failures = inspect_instructions(disassembly, start, end)
    args.output.with_suffix('.asm').write_text(disassembly, encoding='utf-8')
    result = {'elf': str(args.elf), 'sha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
              'range_start': hex(start), 'range_end': hex(end), 'range_bytes': end - start,
              'kind': args.kind,
              'decoded_instructions': instructions, 'indirect_calls': calls,
              'external_reference_check_passed': not failures, 'failures': failures,
              'scope': 'Final copied instructions only; no target execution, supervisor integration or gameplay proof.'}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
