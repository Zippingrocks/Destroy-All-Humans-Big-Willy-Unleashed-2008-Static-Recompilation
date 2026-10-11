"""Resolve observed callback edges against the original symbol map."""
import argparse, bisect, csv, json
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('logs',nargs='+',type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
symbols=[]
for line in (root/'workstation/symbols/main.map').read_text().splitlines():
    address,size,name=line.split(' ',2)
    symbols.append((int(address,16),int(size,16),name))
symbols.sort()
starts=[s[0] for s in symbols]
def label(address):
    i=bisect.bisect_right(starts,address)-1
    if i>=0 and address<symbols[i][0]+symbols[i][1]:
        base,_,name=symbols[i]
        return name+(f'+0x{address-base:x}' if address!=base else '')
    return None
for log in a.logs:
    edges={}
    with (log/'callback-edges.csv').open() as source:
        for row in csv.DictReader(source):
            if row['site']=='site':
                continue  # Earlier trace builds repeated the header after CPU pauses.
            key=(row['kind'],row['site'],row['target'])
            if key not in edges or row['native_verified']=='0':
                edges[key]=row
    resolved=[dict(row,site_symbol=label(int(row['site'],16)),target_symbol=label(int(row['target'],16)))
              for row in edges.values()]
    summary={'observed_unique_edges':len(resolved),
             'unique_targets':len({r['target'] for r in resolved}),
             'not_native_verified':[r for r in resolved if r['native_verified']=='0'],
             'unmapped_targets':[r for r in resolved if r['target_symbol'] is None],
             'verification_note':'native_verified=0 may mean a chunk awaits lazy first-use verification, not a missing callback.',
             'limitation':'Observed CTR calls only; not proof of campaign-wide callback correctness.'}
    (log/'callback-summary.json').write_text(json.dumps(summary,indent=2))
    (log/'callback-resolved.json').write_text(json.dumps(resolved,indent=2))
    print(log.name, json.dumps({'edges':summary['observed_unique_edges'],
          'targets':summary['unique_targets'], 'not_cached_verified':len(summary['not_native_verified']),
          'unmapped_targets':len(summary['unmapped_targets'])}))
