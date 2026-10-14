import struct
from xmap import load
B=0x803a6000
def blk(td,o):
    c,a,d=struct.unpack_from('<III',td,o); return c,a
def tdata(td,o):
    size,fl,fo,addr,d=struct.unpack_from('<IIIII',td,o); return size,addr
def graph(td,addr):
    go=addr-B
    nc,na=blk(td,go+0x68); ac,aa=blk(td,go+0x74)
    nodes=[]
    for i in range(nc):
        o=na-B+i*0x40
        nodes.append(dict(name=td[o:o+32].split(b'\0')[0].decode(),parent=struct.unpack_from('<h',td,o+0x24)[0]))
    anims=[]
    for i in range(ac):
        o=aa-B+i*0xb4
        d=dict(i=i,name=td[o:o+32].split(b'\0')[0].decode())
        d['type'],d['fc'],d['fs'],d['fit']=struct.unpack_from('<4h',td,o+0x20)
        d['checksum']=struct.unpack_from('<I',td,o+0x28)[0]
        d['nc'],d['loop']=struct.unpack_from('<2h',td,o+0x2c)
        d['flags']=struct.unpack_from('<H',td,o+0x3a)[0]
        d['finfo']=tdata(td,o+0x48)
        d['tflags']=struct.unpack_from('<2I',td,o+0x5c); d['rflags']=struct.unpack_from('<2I',td,o+0x6c); d['sflags']=struct.unpack_from('<2I',td,o+0x7c)
        d['cdo']=struct.unpack_from('<I',td,o+0x88)[0]
        d['default']=tdata(td,o+0x8c); d['data']=tdata(td,o+0xa0)
        anims.append(d)
    return nodes,anims
if __name__=='__main__':
    full,off,size,td,h=load('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map')
    nodes,anims=graph(td,0x8072bd4c)
    print(len(nodes),[n['name'] for n in nodes])
    for a in anims:
        if 'walk' in a['name'] or 'run' in a['name'] or a['i']<6: print(a['i'],a['name'],a['type'],a['fc'],a['fs'],a['nc'],hex(a['flags']),a['data'][0],a['default'][0],a['cdo'])
    print(len(anims))
