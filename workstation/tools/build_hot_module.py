"""Build a separate strict-FP compiler experiment from unchanged generated code."""
from pathlib import Path
import subprocess, concurrent.futures, json, argparse, shutil

parser=argparse.ArgumentParser()
parser.add_argument('--all', action='store_true', help='Compile every generated chunk')
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--reuse-hot', action='store_true', help='Reuse the unchanged hot experiment objects (same headers and compiler required)')
parser.add_argument('--thin-lto', action='store_true', help='Build a separate cross-file optimization experiment')
a=parser.parse_args()

root=Path(__file__).resolve().parents[2]
w=root/'workstation'
out=w/('build/game-module-clang-lto' if a.thin_lto else ('build/game-module-clang-all' if a.all else 'build/game-module-clang-hot'))
out.mkdir(exist_ok=True)
engine=root/'github repos/ModernGekko-Template/lib/ModernGekko/vendor/dolphin'
generated=w/'source code tree/runtime-generated/RDHE78_generated'
clang=Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe')
selected=set()
if not a.all:
    profile=json.loads((w/'logs/gameplay-perf-20260912-165201/cpu-samples.resolved.json').read_text())
    selected={name for name,n in profile['objects'] if name.startswith('chunk_')}
selected.add('cpu_interpreter_float.c.obj')
original=sorted((w/'build/game-module/CMakeFiles/gRDHE78_recomp.dir').rglob('*.obj'),
    key=lambda o:(o.name!='module_export.c.obj', o.name.startswith('chunk_'),o.name))
if not original or original[0].name!='module_export.c.obj':
    raise RuntimeError('Build workstation/build-game.cmd before the optimized module')
if len({o.name for o in original})!=len(original):
    raise RuntimeError('Ambiguous original object files; refusing to combine build trees')
if a.all:
    selected.update(o.name for o in original if o.name.startswith('chunk_'))
def compile(obj):
    source=(generated/'chunks'/obj.name[:-4] if obj.name.startswith('chunk_')
            else engine/'GXRuntime/src/core'/obj.name[:-4])
    target=out/obj.name
    cached=w/'build/game-module-clang-hot'/obj.name
    if a.all and a.reuse_hot and not a.thin_lto and cached.exists() and cached.stat().st_mtime > source.stat().st_mtime:
        shutil.copy2(cached,target)
        return
    args=[str(clang),'/nologo','/c','/O2','/Ob2','/MD','/std:c11','/fp:strict','/arch:AVX2',
          '/DWIN32','/D_WINDOWS','/DNDEBUG','/DDOLRECOMP_CPU_HEADER="core/cpu.h"',
          '/I'+str(generated),'/I'+str(engine/'GXRuntime/include'),
          '/I'+str(engine/'Source/Core/Core/PowerPC/StaticRecomp'),
          '/I'+str(w/'build/game-module'),'/Fo'+str(target),str(source)]
    if a.thin_lto:
        args += ['/clang:-flto=thin']
    result=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    target.with_suffix('.log').write_text(result.stdout)
    if result.returncode: raise RuntimeError(f'Compile failed: {source}; see {target.with_suffix(".log")}')
    print('compiled '+obj.name,flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
    list(pool.map(compile,[o for o in original if o.name in selected]))
objects=[out/o.name if o.name in selected else o for o in original]
(out/'link.rsp').write_text('\n'.join('"'+str(o)+'"' for o in objects))
args=['link','/NOLOGO','/DLL','/MACHINE:X64','/INCREMENTAL:NO',
      '/OUT:'+str(out/'gRDHE78_recomp.dll'),'/MAP:'+str(out/'game.map'),
      '@'+str(out/'link.rsp'),'kernel32.lib','user32.lib','gdi32.lib','winspool.lib',
      'shell32.lib','ole32.lib','oleaut32.lib','uuid.lib','comdlg32.lib','advapi32.lib']
if a.thin_lto:
    args[0]=str(clang.with_name('lld-link.exe'))
    args += ['/opt:lldltojobs=8']
with (out/'link.log').open('w') as log:
    subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,check=True)
print(out/'gRDHE78_recomp.dll',flush=True)
(out/'build-settings.json').write_text(json.dumps({'compiler':str(clang),
    'flags':['/O2','/Ob2','/MD','/std:c11','/fp:strict','/arch:AVX2'],
    'requires_avx2':True,'all_chunks':a.all,'thin_lto':a.thin_lto,'objects':len(objects)},indent=2))
