import pefile,sys,json
from capstone import *
pe=pefile.PE(__import__('os').environ.get('APGAME','APGame.exe'),fast_load=True); d=pe.get_memory_mapped_image(); b=pe.OPTIONAL_HEADER.ImageBase
md=Cs(CS_ARCH_X86,CS_MODE_32)
nat={int(v,16):k for k,v in json.load(open(__import__('os').path.join(__import__('os').path.dirname(__file__),'apgame_natives.json'))).items()}
def dis(va,n):
    out=[]
    for i in md.disasm(d[va-b:va-b+n],va):
        s='%08x %-7s %s'%(i.address,i.mnemonic,i.op_str)
        if i.mnemonic=='call':
            try:
                t=int(i.op_str,16); s+='   ; '+nat.get(t,'')
            except: pass
        out.append(s)
        if i.mnemonic=='ret' and len(out)>3 and n<0x200: break
        if i.mnemonic=='int3': break
    return out
for a in sys.argv[1:]:
    va=int(a.split(':')[0],16); n=int(a.split(':')[1],16) if ':' in a else 0x120
    print('=====',hex(va),nat.get(va,'')); print('\n'.join(dis(va,n)))
