"""Relink the verified all-Clang game objects with repaired module callback glue."""
from pathlib import Path
import hashlib, json, subprocess

root=Path(__file__).resolve().parents[2]
w=root/'workstation'
baseline=w/'build/game-module-clang-all'
out=w/'build/game-module-callback-audit'
out.mkdir(exist_ok=True)
engine=root/'github repos/ModernGekko-Template/lib/ModernGekko/vendor/dolphin'
generated=w/'source code tree/runtime-generated/RDHE78_generated'
objects=[Path(line.strip().strip('"')) for line in (baseline/'link.rsp').read_text().splitlines() if line.strip()]
assert objects[0].name=='module_export.c.obj' and all(x.is_file() for x in objects)
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
assert digest(baseline/'gRDHE78_recomp.dll') == digest(root/'dist/flagship/gRDHE78_recomp.dll'), 'Baseline is not the shipped module'
objects[0]=out/'module_export.c.obj'
args=['cl','/nologo','/c','/O2','/Ob2','/MD','/std:c11','/fp:strict',
      '/DWIN32','/D_WINDOWS','/DNDEBUG','/DDOLRECOMP_CPU_HEADER="core/cpu.h"','/DMODULE_GAME_ID="RDHE78"',
      '/I'+str(generated),'/I'+str(engine/'GXRuntime/include'),
      '/I'+str(engine/'Source/Core/Core/PowerPC/StaticRecomp'),
      '/I'+str(w/'build/game-module'),'/Fo'+str(objects[0]),str(engine/'module-template/module_export.c')]
subprocess.run(args,check=True)
(out/'link.rsp').write_text('\n'.join('"'+str(x)+'"' for x in objects))
subprocess.run(['link','/NOLOGO','/DLL','/MACHINE:X64','/INCREMENTAL:NO',
               '/OUT:'+str(out/'gRDHE78_recomp.dll'),'/MAP:'+str(out/'game.map'),
               '@'+str(out/'link.rsp'),'kernel32.lib','user32.lib','gdi32.lib','winspool.lib',
               'shell32.lib','ole32.lib','oleaut32.lib','uuid.lib','comdlg32.lib','advapi32.lib'],check=True)
(out/'build-settings.json').write_text(json.dumps({'baseline':str(baseline),
    'baseline_sha256':digest(baseline/'gRDHE78_recomp.dll'), 'changed_object':'module_export.c.obj',
    'reused_objects':len(objects)-1, 'sha256':digest(out/'gRDHE78_recomp.dll')},indent=2))
print(out/'gRDHE78_recomp.dll',flush=True)
