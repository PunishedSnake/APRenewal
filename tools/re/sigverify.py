"""Check APRenewal's exe lookups (signatures, UE3 natives, vtable slot) against one or more APGame.exe builds.
Usage: python3 -I sigverify.py <GOG APGame.exe> <Steam APGame.exe> ... - every line must show exactly one hit."""
import pefile, re, struct
def load(f):
    p=pefile.PE(f,fast_load=True); d=bytes(p.get_memory_mapped_image()); b=p.OPTIONAL_HEADER.ImageBase
    secs={s.Name.rstrip(b'\0').decode():(s.VirtualAddress,s.Misc_VirtualSize) for s in p.sections}
    return d,b,secs
def pat(s):
    return re.compile(b''.join(b'.' if t=='?' else re.escape(bytes([int(t,16)])) for t in s.split()), re.S)
def findall(img,s):
    d,b,secs=img; va,sz=secs['.text']; return [b+va+m.start() for m in pat(s).finditer(d[va:va+sz])]
def native(img,cls,fn):
    d,b,secs=img; want=('int%sexec%s'%(cls,fn)).encode('utf-16le')+b'\0\0'
    tv,ts=secs['.text']
    for sec in ('.rdata','.data'):
        va,sz=secs[sec]; blob=d[va:va+sz]; i=blob.find(want)
        while i>=0:
            if i%2==0 and (i<2 or blob[i-2]==0):
                na=b+va+i
                for tsec in ('.data','.rdata'):
                    tva,tsz=secs[tsec]; tb=d[tva:tva+tsz]; key=struct.pack('<I',na); j=tb.find(key)
                    while j>=0:
                        if j%4==0:
                            f=struct.unpack_from('<I',tb,j+4)[0]
                            if b+tv<=f<b+tv+ts: return f
                        j=tb.find(key,j+1)
            i=blob.find(want,i+1)
movie="55 8B EC 83 E4 F8 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 81 EC 00 02 00 00 A1 ? ? ? ? 33 C4 89 84 24 F8 01 00 00 53 56 57 A1 ? ? ? ? 33 C4 50 8D 84 24 10 02 00 00 64 A3 00 00 00 00 8B 7D 0C 8B D9 83 7B 24 00 89 7C 24 10 75 ? C7 43 24 01 00 00 00 8B 45 08 89 43 08 A1"
pa="A1 ? ? ? ? 85 C0 74 ? F6 80 3C 01 00 00 01 74 ? 81 A7 FC 01 00 00 FF F7 FF FF"
pb="A1 ? ? ? ? 85 C0 74 ? F6 80 3C 01 00 00 01 74 ? 81 A6 FC 01 00 00 FF F7 FF FF"
views=["F3 0F 59 80 0C 01 00 00 09 98 10 01 00 00","89 53 70 8B 15 ? ? ? ? 89 4B 64 89 43 68 8B 52 50","51 56 8B F1 33 C0 39 46 44 57 74 ? 8B 4E 44 8B 01 8B 50 08 FF D2 85 C0 89 44 24 08 DB 44 24 08"]
import sys
for f in sys.argv[1:]:
    name=f
    img=load(f); d,b,secs=img
    m=findall(img,movie); print(name,'movie',[hex(x) for x in m])
    rva,rsz=secs['.rdata']; rd=d[rva:rva+rsz]
    key=struct.pack('<I',m[0]); slots=[hex(b+rva+j) for j in range(0,len(rd)-3,4) if rd[j:j+4]==key]; print('  slots',slots)
    a=findall(img,pa); bb=findall(img,pb); print('  pad',[hex(x) for x in a],[hex(x) for x in bb], [hex(struct.unpack_from('<I',d,x-b+1)[0]) for x in a+bb])
    cc=native(img,'APlayerController','ConsoleCommand'); print('  cc',hex(cc), d[cc-b+0xB7:cc-b+0xBF].hex(), d[cc-b+0xF1:cc-b+0xF3].hex(), hex(struct.unpack_from('<I',d,cc-b+0xF3)[0]), d[cc-b+0x108:cc-b+0x10D].hex())
    for fn in ('ShouldAllowPauseMenu','ShouldAllowPDAMenu','IsPlayerPastTitleScreen','UpdateAIData'):
        x=native(img,'AAPPlayerController',fn); print('  ',fn,hex(x),d[x-b:x-b+8].hex())
    for v in views: print('  view',[hex(x) for x in findall(img,v)][:4])
