import pefile, re, struct, sys, json
pe=pefile.PE(sys.argv[1] if len(sys.argv) > 1 else 'APGame.exe',fast_load=True); base=pe.OPTIONAL_HEADER.ImageBase
d=pe.get_memory_mapped_image()
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
t0=base+text.VirtualAddress; t1=t0+text.Misc_VirtualSize
names={}
for m in re.finditer(rb'i\x00n\x00t\x00((?:[A-Za-z0-9_]\x00)+?)e\x00x\x00e\x00c\x00((?:[A-Za-z0-9_]\x00)+)\x00\x00', d):
    names[base+m.start()]=(m.group(1).decode('utf-16le'), m.group(2).decode('utf-16le'))
# pointer table scan over data sections
res={}
for s in pe.sections:
    if s.Name.startswith(b'.text'): continue
    a=s.VirtualAddress; b=a+s.Misc_VirtualSize
    for off in range(a, b-8, 4):
        p=struct.unpack_from('<I',d,off)[0]
        if p in names:
            f=struct.unpack_from('<I',d,off+4)[0]
            if t0<=f<t1: res['%s::exec%s'%names[p]]=f
print(len(names),'names,',len(res),'resolved')
json.dump({k:hex(v) for k,v in sorted(res.items())}, open('apgame_natives.json','w'), indent=0)
