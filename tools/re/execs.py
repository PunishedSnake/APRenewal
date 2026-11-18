import sys,struct; sys.path.insert(0,__import__('os').path.dirname(__file__))
from upk import Pkg
FUNC_Exec=0x200; FUNC_Net=0x40; FUNC_Native=0x400; FUNC_Static=0x2000
def funcs(path):
    p=Pkg(path); out=[]
    for i,e in enumerate(p.exports):
        if p.clsname(e)!='Function': continue
        d=p.d[e['off']:e['off']+e['size']]
        for tail in (12,14):
            fl=struct.unpack_from('<I',d,len(d)-tail)[0]
            fn=struct.unpack_from('<i',d,len(d)-8)[0]
            if (tail==14)==bool(fl&FUNC_Net) and 0<=fn<len(p.names): break
        out.append((p.path(i+1),fl,p.names[fn] if 0<=fn<len(p.names) else '?',d))
    return p,out
if __name__=='__main__':
    for f in sys.argv[1:]:
        p,fs=funcs(f); bad=sum(1 for a,fl,fn,d in fs if fn!=a.split('.')[-1])
        print('==',f,len(fs),'friendly mismatch',bad)
        for a,fl,fn,d in fs:
            if fl&FUNC_Exec: print('  %-60s %08x'%(a,fl))
