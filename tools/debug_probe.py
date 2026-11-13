# The remote console (debug_remote.c) from Python: run(['player', 'area']) drops the commands in debug_cmd.txt next to
# the game and returns debug_out.txt's answer. NX2NAVI_GAME is the folder the game runs from.
import sys,time,os,re
G=os.environ.get('NX2NAVI_GAME') or sys.exit('set NX2NAVI_GAME to the folder the game runs from')
def run(cmds,timeout=180):
    o=os.path.join(G,'debug_out.txt')
    if os.path.exists(o): os.remove(o)
    open(os.path.join(G,'debug_cmd.txt'),'w').write('\n'.join(cmds)+'\n')
    t=time.time()
    while time.time()-t<timeout:
        if os.path.exists(o): return open(o,encoding='latin-1').read()
        time.sleep(0.5)
    return 'TIMEOUT'
