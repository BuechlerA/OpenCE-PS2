import zlib,struct
def load(path):
    d=open(path,'rb').read()
    full=d[:0x800]+zlib.decompressobj().decompress(d[0x800:])
    ver,flen,_,off,size=struct.unpack_from('<5i',full,4)
    td=full[off:off+size]
    ti,scen,chk,cnt,vbc,vb,ibc,ib,sig=struct.unpack_from('<IiIiIIIII',td,0)
    return full,off,size,td,(ti,scen,cnt,vbc,vb,ibc,ib)
if __name__=='__main__':
    full,off,size,td,h=load('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map')
    print([hex(x) for x in h], hex(off),hex(size),len(full))
    ti=h[0]
    for i in range(h[2]):
        g,p0,p1,idx,name,base,u0,u1=struct.unpack_from('<4s4s4sIIIII',td,0x28+i*0x20) if False else (None,)*8
    # instances at tag_instances pointer; guess base = ti - 0x28
    base=ti-0x28
    print(hex(base))
    for i in range(h[2]):
        o=0x28+i*0x20
        g=td[o:o+4][::-1]; idx,nm,ba=struct.unpack_from('<III',td,o+0x10)
        if nm-base<len(td):
            n=td[nm-base:td.index(b'\0',nm-base)].decode('latin1')
        else: n='?'
        if 'cyborg' in n or 'chief' in n: print(i,g,hex(ba),n)
