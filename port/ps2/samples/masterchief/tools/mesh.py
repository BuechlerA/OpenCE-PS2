import struct,sys
from xmap import load
from extract import extract,blk,B
def decode_vec(u):
    # Xbox compressed normal: 11-11-10 signed
    x=u&0x7ff; y=(u>>11)&0x7ff; z=(u>>22)&0x3ff
    def s(v,bits):
        m=1<<(bits-1)
        if v&m: v-=1<<bits
        return v/ (m-1)
    return (s(x,11),s(y,11),s(z,10))
def get_mesh(path, addr, geom=0):
    full,td,mo,shaders,regions,(gc,ga)=extract(path,addr)
    go=ga-B+geom*0x30
    pc,pa=blk(td,go+0x24)
    parts=[]
    for p in range(pc):
        po=pa-B+p*0x68
        flags,shader=struct.unpack_from('<IH',td,po)
        ttype,tcount,tbase=struct.unpack_from('<HxxII',td,po+0x44)
        vtype,vcount,voff,vbase,vhw=struct.unpack_from('<HxxIIII',td,po+0x54)
        vdata=struct.unpack_from('<I',td,vhw-B+4)[0]-B
        if tbase>=B: idx=struct.unpack_from('<%dH'%(tcount+2 if ttype==1 else tcount*3),td,tbase-B)
        else: idx=None
        verts=[]
        for i in range(vcount):
            raw=struct.unpack_from('<3fIIIhhBBh',td,vdata+i*32)
            verts.append(raw)
        parts.append(dict(flags=flags,shader=shaders[shader] if shader<len(shaders) else None,ttype=ttype,idx=idx,vtype=vtype,verts=verts))
    return parts
if __name__=='__main__':
    parts=get_mesh('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map',0x80729b64)
    for p in parts:
        v=p['verts']
        xs=[a[0] for a in v]; ys=[a[1] for a in v]; zs=[a[2] for a in v]
        print(p['shader'],p['vtype'],len(v),p['ttype'],len(p['idx']),max(p['idx']) if p['idx'] else None,(min(xs),max(xs)),(min(ys),max(ys)),(min(zs),max(zs)))
