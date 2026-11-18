import sys,struct,re; sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from upk import Pkg
PATH=__import__('os').environ.get('AP_SHADERCACHE','RefShaderCache-PC-D3D-SM3.upk')
p=Pkg(PATH); c=open(PATH,'rb').read(); N=len(p.names)
def nm(o):
    i,n=struct.unpack_from('<ii',c,o)
    return p.names[i] if 0<=i<N and n==0 else None
def fnv(b):
    h=2166136261
    for x in b: h=((h^x)*16777619)&0xffffffff
    return h
def dxlen(o):
    i=o+4
    while True:
        tok=struct.unpack_from('<I',c,i)[0]
        if tok==0xFFFF: return i+4-o
        if tok&0xFFFF==0xFFFE: i+=4+((tok>>16)&0x7FFF)*4
        else: i+=4+((tok>>24)&0xF)*4
_shaders=None
def shaders():
    global _shaders
    if _shaders is None:
        _shaders={}
        for m in re.finditer(rb'(\x00\x03\xff\xff|\x00\x03\xfe\xff)',c):
            o=m.start(); h=o-34
            if h<0 or nm(h) is None: continue
            size=struct.unpack_from('<I',c,o-4)[0]
            if not 16<size<200000: continue
            guid=c[h+8:h+24]
            code=c[o:o+dxlen(o)]
            _shaders[guid]=(nm(h),fnv(code),o,len(code))
    return _shaders
def shadermaps(baseguid):
    """yield (offset, [(vf, shadertype, guid)]) for shader maps whose static base is baseguid"""
    for m in re.finditer(re.escape(baseguid),c):
        o=m.start()+16
        try:
            ns=struct.unpack_from('<i',c,o)[0]; o+=4
            if not 0<=ns<64: continue
            o+=32*ns
            nm_=struct.unpack_from('<i',c,o)[0]; o+=4
            if not 0<=nm_<64: continue
            o+=44*nm_
            o+=8
            nvf=struct.unpack_from('<i',c,o)[0]; o+=8
            if not 0<nvf<64: continue
            out=[]
            for k in range(nvf):
                vf=nm(o); o+=8
                if vf is None: raise ValueError
                cnt=struct.unpack_from('<i',c,o)[0]; o+=4
                if not 0<=cnt<200: raise ValueError
                for j in range(cnt):
                    st=nm(o); g=c[o+8:o+24]; o+=32
                    out.append((vf,st,g))
            yield m.start(),out
        except Exception: continue

def material_maps():
    """[(friendly_name, materialId, [(shadertype, hash)])] using the friendly-name strings that close each map"""
    S=shaders()
    # shader references: name(8) guid(16) same name(8)
    refs=[]
    for m in re.finditer(rb'(?=(.{4})\x00\x00\x00\x00(.{16})\1\x00\x00\x00\x00)',c,re.S):
        g=m.group(2)
        if g in S and m.start()+8!=S[g][2]-26: refs.append((m.start()+8,S[g][0],S[g][1]))
    refs.sort()
    # friendly names: guid(16) + int len + ascii + NUL, preceded by a name (8)
    names=[]
    for m in re.finditer(rb'[\x05-\x60]\x00\x00\x00([ -~]{4,95})\x00',c):
        o=m.start(); L=c[o]
        if len(m.group(1))+1!=L: continue
        names.append((o,m.group(1).decode(),c[o-16:o]))
    out=[]; j=0; prev=0
    for o,nm_,gid in names:
        lst=[]
        while j<len(refs) and refs[j][0]<o:
            if refs[j][0]>prev: lst.append(refs[j][1:])
            j+=1
        if lst: out.append((nm_,gid,lst))
        prev=o
    return out
