import sys,time,os,re
G=r'C:\Users\Justin\NX2navi-dev\game'
def run(cmds,timeout=180):
    o=os.path.join(G,'debug_out.txt')
    if os.path.exists(o): os.remove(o)
    open(os.path.join(G,'debug_cmd.txt'),'w').write('\n'.join(cmds)+'\n')
    t=time.time()
    while time.time()-t<timeout:
        if os.path.exists(o): return open(o,encoding='latin-1').read()
        time.sleep(0.5)
    return 'TIMEOUT'
