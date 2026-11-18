import struct,sys; sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from upk import Pkg
def tags(p,e,start=4):
    d=p.d[e['off']:e['off']+e['size']]; o=start; out=[]
    while o+8<=len(d):
        ni=struct.unpack_from('<i',d,o)[0]
        if not 0<=ni<len(p.names): break
        n=p.names[ni]; o+=8
        if n=='None': break
        t=p.names[struct.unpack_from('<i',d,o)[0]]; o+=8
        sz,ai=struct.unpack_from('<ii',d,o); o+=8
        if t=='StructProperty': sn=p.names[struct.unpack_from('<i',d,o)[0]]; o+=8
        else: sn=None
        if t=='BoolProperty': v=struct.unpack_from('<i',d,o)[0]; o+=4; out.append((n,t,v)); continue
        raw=d[o:o+sz]; o+=sz
        if t=='FloatProperty': v=struct.unpack('<f',raw)[0]
        elif t=='IntProperty': v=struct.unpack('<i',raw)[0]
        elif t=='ObjectProperty': v=p.path(struct.unpack('<i',raw)[0])
        elif t=='NameProperty': v=p.names[struct.unpack('<i',raw[:4])[0]]
        elif t=='ByteProperty': v=raw[0] if sz==1 else p.names[struct.unpack('<i',raw[:4])[0]]
        elif t=='StrProperty':
            l=struct.unpack_from('<i',raw)[0]; v=raw[4:4+l-1].decode('latin1') if l>0 else raw[4:4-2*l-2].decode('utf-16le')
        elif t=='StructProperty' and sn in ('ExpressionInput','ColorMaterialInput','ScalarMaterialInput','VectorMaterialInput','Vector2MaterialInput'):
            v=(sn, p.path(struct.unpack_from('<i',raw)[0]))
        elif t=='StructProperty' and sn=='LinearColor': v=struct.unpack('<4f',raw[:16])
        else: v=(sn,sz)
        out.append((n if ai==0 else '%s[%d]'%(n,ai),t,v))
    return out
