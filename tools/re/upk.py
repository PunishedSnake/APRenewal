import struct,sys
class Pkg:
    def __init__(s,path):
        d=s.d=open(path,'rb').read(); o=8
        s.hdr=struct.unpack_from('<I',d,o)[0]; o+=4
        fl=struct.unpack_from('<i',d,o)[0]; o+=4+fl
        s.flags,s.nn,s.no,s.ne,s.eo,s.ni,s.io,s.do=struct.unpack_from('<IiiiiiiI',d,o); o+=32
        o+=16  # guid
        gc=struct.unpack_from('<i',d,o)[0]; o+=4+12*gc
        s.engv,s.cookv,s.comp,s.nchunks=struct.unpack_from('<iiii',d,o)
        if s.comp:
            import zlib
            o2=o+16; ch=[struct.unpack_from('<iiii',d,o2+16*k) for k in range(s.nchunks)]
            end=max(a+b for a,b,c,e in ch); out=bytearray(end); out[:ch[0][0]]=d[:ch[0][0]]
            for uo,us,co,cs in ch:
                q=co+8; ctot,utot=struct.unpack_from('<ii',d,q); q+=8
                blocks=[]; got=0
                while got<utot:
                    a,b=struct.unpack_from('<ii',d,q); q+=8; blocks.append((a,b)); got+=b
                w=uo
                for a,b in blocks:
                    out[w:w+b]=zlib.decompress(d[q:q+a]); q+=a; w+=b
            d=s.d=bytes(out)
        s.names=[]; o=s.no
        for i in range(s.nn):
            l=struct.unpack_from('<i',d,o)[0]; o+=4
            if l<0: nm=d[o:o-2*l].decode('utf-16le')[:-1]; o+=-2*l
            else: nm=d[o:o+l].decode('latin1')[:-1]; o+=l
            o+=8; s.names.append(nm)
        s.imports=[]; o=s.io
        for i in range(s.ni):
            cp,cpn,cn,cnn,outer,on,onn=struct.unpack_from('<iiiiiii',d,o); o+=28
            s.imports.append((s.names[cn],outer,s.names[on]))
        s.exports=[]; o=s.eo
        for i in range(s.ne):
            cls,sup,outer,on,onn,arch,oflh,ofll,ssz,sof,eflags,ngen=struct.unpack_from('<iiiiiiIIiiIi',d,o); o+=48
            o+=12*ngen  # net object counts? (v539: generation net object count array of ints)
            o-=12*ngen; o+=4*ngen
            o+=16+4 # guid + package flags
            s.exports.append(dict(cls=cls,sup=sup,outer=outer,name=s.names[on],num=onn,size=ssz,off=sof))
    def objname(s,i):
        if i>0: return s.exports[i-1]['name']
        if i<0: return s.imports[-i-1][2]
        return 'None'
    def clsname(s,e): return s.objname(e['cls']) if e['cls'] else 'Class'
    def path(s,i):
        parts=[]
        while i:
            if i>0: e=s.exports[i-1]; parts.append(e['name']); i=e['outer']
            else: im=s.imports[-i-1]; parts.append(im[2]); i=im[1]
        return '.'.join(reversed(parts))
