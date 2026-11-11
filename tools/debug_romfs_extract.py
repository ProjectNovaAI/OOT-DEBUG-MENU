import json,struct,zstandard
d=json.load(open(r'C:\Users\Justin\NX2navi-dev\game\romfs\index.json',encoding='utf-8'))
idx={f['path']:f for f in d['files']}
def raw(path):
    f=idx[path]
    with open(r'C:\Users\Justin\NX2navi-dev\game\romfs\romfs.bin','rb') as fh:
        fh.seek(f['offset']); return fh.read(f['size'])
def sarc(b):
    assert b[:4]==b'SARC'
    bom=b[6:8]; e='<' if bom==b'\xff\xfe' else '>'
    dataoff=struct.unpack(e+'I',b[0xc:0x10])[0]
    assert b[0x14:0x18]==b'SFAT'
    n=struct.unpack(e+'H',b[0x1a:0x1c])[0]
    sfat=0x20; sfnt=sfat+n*16
    nameoff=sfnt+8
    out={}
    for i in range(n):
        h,fa,s,en=struct.unpack(e+'IIII',b[sfat+i*16:sfat+i*16+16])
        no=(fa&0xffffff)*4+nameoff
        name=b[no:b.index(b'\0',no)].decode()
        out[name]=b[dataoff+s:dataoff+en]
    return out
_z=zstandard.ZstdDecompressor()
files=sarc(_z.decompress(raw('Pack/ZsDic.pack.zs'),max_output_size=1<<28))
dicts={k:zstandard.ZstdCompressionDict(v) for k,v in files.items()}
def get(path):
    r=raw(path)
    if not path.endswith('.zs'): return r
    for name,dd in dicts.items():
        try: return zstandard.ZstdDecompressor(dict_data=dd).decompress(r,max_output_size=1<<30)
        except Exception: pass
    return _z.decompress(r,max_output_size=1<<30)
if __name__=='__main__':
    import sys
    print(list(files))
    b=get(sys.argv[1]); open(sys.argv[2],'wb').write(b); print(len(b)); print(b[:128].hex(' '))
