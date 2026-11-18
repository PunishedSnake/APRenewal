# Builds src/matclass.h: base-pass pixel shader hashes of skin / metal materials.
# usage: python3 -I matclass.py RefShaderCache-PC-D3D-SM3.upk > ../../src/matclass.h  (see shadercache.PATH)
import sys,re,collections,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import shadercache as sc
if len(sys.argv)>1: pass  # edit shadercache.PATH to point at the cache
def cls(n):
    if re.search(r'skin',n,re.I) and not re.search(r'skinned',n,re.I): return 'skin'
    if re.search(r'^MASTER_Weapon|^MASTER_BasicVehicleWeapon|^AP_Weapon_Master$|^Weapons_MST_MAT$|metal|chrome|steel|^Pipes|Chains|ShellCasing|ShotgunShell|^Shotgun_',n,re.I): return 'metal'
    return None
owners=collections.defaultdict(set)
for n,g,l in sc.material_maps():
    for st,h in l:
        if st.startswith('TBasePassPixelShader'): owners[h].add(cls(n))
res={'skin':[],'metal':[]}
for h,s in owners.items():
    if len(s)==1 and next(iter(s)): res[next(iter(s))].append(h)
print('#pragma once\n#include <cstdint>')
for k in ('skin','metal'):
    v=sorted(res[k]); print('static const uint32_t k%sHashes[%d] = {'%(k.capitalize(),len(v)))
    for i in range(0,len(v),8): print('    '+', '.join('0x%08xu'%h for h in v[i:i+8])+',')
    print('};')
