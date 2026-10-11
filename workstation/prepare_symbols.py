"""Verify ELF executable sections against DOL before exporting function symbols."""
from pathlib import Path
import hashlib
import json
import struct

root = Path(__file__).resolve().parent
elf_path, dol_path = root / 'input/dah_rls.elf', root / 'input/main.dol'
elf, dol = elf_path.read_bytes(), dol_path.read_bytes()
assert elf[:6] == b'\x7fELF\x01\x02', 'Expected big-endian ELF32'
header = struct.unpack_from('>HHIIIIIHHHHHH', elf, 16)
sections = [struct.unpack_from('>IIIIIIIIII', elf, header[5] + i * header[10])
            for i in range(header[11])]
def string(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('utf-8', 'replace')
names_section = sections[header[12]]
names = elf[names_section[4]:names_section[4] + names_section[5]]
dol_sections = []
for i in range(18):
    offset = struct.unpack_from('>I', dol, i * 4)[0]
    address = struct.unpack_from('>I', dol, 0x48 + i * 4)[0]
    size = struct.unpack_from('>I', dol, 0x90 + i * 4)[0]
    if size:
        dol_sections.append((address, size, offset))
checked = []
for section in sections:
    if section[1] != 1 or not section[2] & 4 or not section[5]:
        continue
    address, offset, size = section[3:6]
    match = next((s for s in dol_sections if s[0] <= address and address + size <= s[0] + s[1]), None)
    assert match, f'ELF section not covered by DOL: {address:x}'
    dstart = match[2] + address - match[0]
    assert elf[offset:offset+size] == dol[dstart:dstart+size], f'Code mismatch at {address:x}'
    checked.append({'name': string(names, section[0]), 'address': hex(address), 'bytes': size})
assert checked, 'No executable sections verified'
functions = set()
for section in sections:
    if section[1] != 2:
        continue
    table = sections[section[6]]
    strings = elf[table[4]:table[4]+table[5]]
    for offset in range(section[4], section[4]+section[5], section[9]):
        name, address, size, info, _, index = struct.unpack_from('>IIIBBH', elf, offset)
        if name and info & 15 == 2 and 0 < index < len(sections) and sections[index][2] & 4:
            functions.add((address, size, string(strings, name)))
out = root / 'symbols'
out.mkdir(exist_ok=True)
(out / 'main.map').write_text(''.join(f'{a:08x} {s:08x} {n}\n' for a,s,n in sorted(functions)), encoding='utf-8')
report = {'elf_sha256': hashlib.sha256(elf).hexdigest(), 'dol_sha256': hashlib.sha256(dol).hexdigest(),
          'matching_executable_sections': checked, 'exported_function_symbols': len(functions)}
(out / 'verification.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
