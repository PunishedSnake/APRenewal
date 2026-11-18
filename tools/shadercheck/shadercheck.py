#!/usr/bin/env python3
"""Compile every HLSL raw string in src/main.cpp with D3DXCompileShader under Wine.

Needs: i686-w64-mingw32-gcc, wine, and d3dx9_42.dll (from the game's Binaries) next to this script.
Wine's builtin compiler (vkd3d-shader) rejects some valid SM3 code (dynamic loops/branches), so a
"not implemented" failure is not necessarily a real error - syntax and type errors are.
Usage: tools/shadercheck/shadercheck.py [ShaderName ...]
"""
import os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', '..', 'src', 'main.cpp')).read()
items = re.findall(r'static const char\* (k\w+) = R"HLSL\((.*?)\)HLSL";', src, re.S)
if len(sys.argv) > 1: items = [i for i in items if i[0] in sys.argv[1:]]
out = ['#include <windows.h>', '#include <stdio.h>', '#include <string.h>',
       'typedef HRESULT (WINAPI *PFN)(LPCSTR,UINT,const void*,void*,LPCSTR,LPCSTR,DWORD,void**,void**,void**);',
       'static void* BufPtr(void* b){ return ((void*(WINAPI*)(void*))(*(void***)b)[3])(b);}',
       'static DWORD BufSize(void* b){ return ((DWORD(WINAPI*)(void*))(*(void***)b)[4])(b);}']
for n, s in items:
    lit = '\n'.join('"%s\\n"' % l.replace('\\', '\\\\').replace('"', '\\"') for l in s.split('\n'))
    out.append('static const char* %s = %s;' % (n, lit))
out.append('int main(){ HMODULE m=LoadLibraryA("d3dx9_42.dll"); if(!m){puts("d3dx9_42.dll missing");return 2;}'
           ' PFN c=(PFN)GetProcAddress(m,"D3DXCompileShader"); int fails=0;')
for n, _ in items:
    prof = 'vs_3_0' if n.endswith('VS') else 'ps_3_0'
    out.append(' { void *code=0,*err=0; HRESULT hr=c(%s,(UINT)strlen(%s),0,0,"main","%s",0,&code,&err,0);'
               ' if(hr<0){fails++; printf("FAIL %s 0x%%08lx\\n%%s\\n",hr,err?(char*)BufPtr(err):"");}'
               ' else printf("ok   %-18s %%5lu bytes\\n", BufSize(code)); }' % (n, n, prof, n, n))
out.append(' printf("%d failure(s)\\n",fails); return fails; }')
c = os.path.join(here, 'shtest.c'); exe = os.path.join(here, 'shtest.exe')
open(c, 'w').write('\n'.join(out))
subprocess.check_call(['i686-w64-mingw32-gcc', '-O1', '-o', exe, c])
sys.exit(subprocess.call(['wine', exe], cwd=here, env=dict(os.environ, WINEDEBUG='-all')))
