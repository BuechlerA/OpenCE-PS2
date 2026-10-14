import struct,sys
from xmap import load
B=0x803a6000
def blk(td,o):
    c,a,d=struct.unpack_from('<III',td,o); return c,a
def tagname(td,nm): return td[nm-B:td.index(b'\0',nm-B)].decode('latin1')
def extract(path, model_addr):
    full,off,size,td,h=load(path)
    mo=model_addr-B
    # shaders
    sc,sa=blk(td,mo+0xac+48)
    shaders=[]
    for i in range(sc):
        o=sa-B+i*0x20
        # tag_reference: group(4), name ptr(4), name_len(4), index(4)
        grp,nm,ln,idx=struct.unpack_from('<4sIII',td,o)
        shaders.append(td[nm-B:td.index(b'\0',nm-B)].decode('latin1') if nm else '')
    # regions
    rc,ra=blk(td,mo+0xac+24)
    gc,ga=blk(td,mo+0xac+36)
    regions=[]
    for r in range(rc):
        ro=ra-B+r*0x4c
        name=td[ro:ro+32].split(b'\0')[0].decode()
        pc,pa=blk(td,ro+0x40)
        perms=[]
        for p in range(pc):
            po=pa-B+p*0x58
            pname=td[po:po+32].split(b'\0')[0].decode()
            geoms=struct.unpack_from('<5h',td,po+0x30+0x1c-0x1c+0) # placeholder
            perms.append((pname,po))
        regions.append((name,perms))
    return full,td,mo,shaders,regions,(gc,ga)
if __name__=='__main__':
    full,td,mo,shaders,regions,g=extract('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map',0x80729b64)
    print(shaders)
    for n,p in regions:
        print(n,[x[0] for x in p])
        for pn,po in p: print(pn, td[po:po+0x58].hex())
