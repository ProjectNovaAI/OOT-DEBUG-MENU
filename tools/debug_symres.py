import re,sys,bisect
M='game/build-cache/ocarina.map'
syms=[]
for l in open(M,encoding='latin-1'):
    m=re.match(r'\s+0x([0-9a-f]{16})\s+([A-Za-z_][\w.$@?]*)\s*$',l)
    if m: syms.append((int(m[1],16),m[2]))
syms.sort()
keys=[a for a,_ in syms]
def make(anchor_runtime):
    d=[x for x in syms if x[1]=='dbg_remote_set'][0][0]
    slide=anchor_runtime-d
    def res(a):
        v=a-slide; i=bisect.bisect_right(keys,v)-1
        return f'{syms[i][1]}+{v-syms[i][0]:#x}' if i>=0 and v-syms[i][0]<0x100000 else f'?{a:#x}'
    return res
