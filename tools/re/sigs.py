import sys,struct; sys.path.insert(0,__import__('os').path.dirname(__file__))
from execs import funcs
res=[]
for f in sys.argv[1:]:
    p,fs=funcs(f)
    params={}
    for i,e in enumerate(p.exports):
        c=p.clsname(e)
        if c.endswith('Property') and e['outer']>0 and p.clsname(p.exports[e['outer']-1])=='Function':
            d=p.d[e['off']:e['off']+e['size']]
            pf=struct.unpack_from('<Q',d,24)[0] if len(d)>=32 else 0
            if pf&0x80: params.setdefault(e['outer'],[]).append((c.replace('Property',''),e['name'],i))
    idx={p.path(i+1):i+1 for i,e in enumerate(p.exports) if p.clsname(e)=='Function'}
    for a,fl,fn,d in fs:
        if fl&0x200:
            ps=params.get(idx[a],[]); ps.sort(key=lambda x:x[2])
            # drop return value
            res.append((f.split('/')[-1],a,' '.join('%s:%s'%(t,n) for t,n,_ in ps if n!='ReturnValue')))
for r in res: print('%s|%s|%s'%r)
