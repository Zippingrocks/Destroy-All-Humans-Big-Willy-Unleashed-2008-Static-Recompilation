"""Inventory original RDHE78 callback candidates against generated entry coverage.

Data-section code pointers are candidates, not proof of a registered callback.
Runtime-created pointers and dynamically loaded code require live validation.
"""
import json, re, struct
from pathlib import Path

root = Path(__file__).resolve().parents[2]
w = root / 'workstation'
b = (w / 'input/dah_rls.elf').read_bytes()
assert b[:6] == b'\x7fELF\x01\x02', 'Expected ELF32 big-endian Wii input'
shoff = struct.unpack_from('>I', b, 32)[0]
entsize, count, names_index = struct.unpack_from('>HHH', b, 46)
sections = [struct.unpack_from('>10I', b, shoff+i*entsize) for i in range(count)]
ns = sections[names_index]
names = b[ns[4]:ns[4]+ns[5]]
def name_at(table, offset):
    return table[offset:table.find(b'\0', offset)].decode('utf-8', errors='replace')
code = [(s[3],s[3]+s[5]) for s in sections if s[2]&4 and s[5]]
def executable(address):
    return address%4 == 0 and any(a <= address < z for a,z in code)
functions = []
for s in sections:
    if s[1] != 2: continue
    st = sections[s[6]]
    strings = b[st[4]:st[4]+st[5]]
    for o in range(s[4], s[4]+s[5], s[9]):
        n, address, size, info, other, idx = struct.unpack_from('>IIIBBH', b, o)
        if info&15 == 2 and executable(address):
            functions.append({'address':address,'name':name_at(strings,n),'size':size})
entries = set()
chunks = sorted((w/'source code tree/runtime-generated/RDHE78_generated/chunks').glob('*.c'))
for path in chunks:
    text = path.read_text()
    # First switch is the external dispatcher; later return switches are only shortcuts.
    match = re.search(r'void func_[0-9A-F]+\(CPUState\* ctx\) \{\s*switch \(ctx->pc\) \{(.*?)default:', text, re.S)
    if not match: raise RuntimeError(f'Unrecognized generated dispatcher: {path}')
    entries.update(int(x,16) for x in re.findall(r'case 0x([0-9A-Fa-f]+)u:', match[1]))
symbols = {f['address']:f['name'] for f in functions}
pointers, indirect = [], []
for s in sections:
    if s[1] != 1 or not s[2]&2: continue
    name = name_at(names,s[0])
    for off in range(0,s[5]-3,4):
        word = struct.unpack_from('>I',b,s[4]+off)[0]
        address = s[3]+off
        if s[2]&4:
            if word>>26 == 19 and ((word>>1)&1023) in (16,528):
                indirect.append({'site':address,'raw':word,'kind':'CTR' if ((word>>1)&1023)==528 else 'LR','link':bool(word&1)})
        elif executable(word):
            pointers.append({'slot':address,'section':name,'target':word,'symbol':symbols.get(word),'covered':word in entries})
missing = [f for f in functions if f['address'] not in entries]
summary = {'chunks':len(chunks),'entry_addresses':len(entries),'named_functions':len(functions),
           'missing_function_entries':len(missing),'static_code_pointer_candidates':len(pointers),
           'uncovered_pointer_candidates':sum(not p['covered'] for p in pointers),
           'indirect_branch_sites':len(indirect),'indirect_linked_calls':sum(p['link'] for p in indirect)}
result={'summary':summary,'missing_functions':missing,'data_code_pointers':pointers,'indirect_branches':indirect}
out=w/'logs/callback-audit-20261011'
out.mkdir(exist_ok=True)
(out/'static-inventory.json').write_text(json.dumps(result,indent=2))
(out/'indirect-sites.txt').write_text(''.join(f"{x['site']:08x} {x['raw']:08x}\n" for x in indirect))
print(json.dumps(summary,indent=2))
if missing: print(json.dumps(missing[:20],indent=2))
