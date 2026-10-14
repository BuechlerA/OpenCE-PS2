import struct
B=0x803a6000
def bitmap_info(td,addr):
    bo=addr-B
    typ,fmt,usage,flags=struct.unpack_from('<hhhH',td,bo)
    psize,pflags,poff=struct.unpack_from('<III',td,bo+0x30)
    cnt,ba,_=struct.unpack_from('<III',td,bo+0x60)
    out=[]
    for i in range(cnt):
        eo=ba-B+i*0x30
        sig,w,h,d,t,f,fl=struct.unpack_from('<4shhhhhH',td,eo)
        mips,_pad,po,ps=struct.unpack_from('<hhII',td,eo+0x14)
        out.append(dict(sig=sig,w=w,h=h,d=d,type=t,fmt=f,flags=fl,mips=mips,pixoff=po,pixsize=ps,filebase=poff))
    return out
if __name__=='__main__':
    from xmap import load
    full,off,size,td,h=load('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map')
    for a in (0x8072b524,0x8072b900,0x8072b9dc):
        print(bitmap_info(td,a))
