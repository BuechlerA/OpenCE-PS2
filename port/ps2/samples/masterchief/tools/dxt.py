import numpy as np
def decode_dxt1(data,w,h,dxt5=False):
    bw,bh=max(1,(w+3)//4),max(1,(h+3)//4)
    bs=16 if dxt5 else 8
    arr=np.frombuffer(data,dtype=np.uint8,count=bw*bh*bs).reshape(bh*bw,bs)
    cb=arr[:,8:] if dxt5 else arr
    c0=cb[:,0].astype(np.uint16)|(cb[:,1].astype(np.uint16)<<8)
    c1=cb[:,2].astype(np.uint16)|(cb[:,3].astype(np.uint16)<<8)
    def rgb(c):
        r=((c>>11)&31)*255//31; g=((c>>5)&63)*255//63; b=(c&31)*255//31
        return np.stack([r,g,b],-1).astype(np.int32)
    p0,p1=rgb(c0),rgb(c1)
    pal=np.zeros((len(c0),4,4),np.int32)
    pal[:,0,:3]=p0; pal[:,1,:3]=p1; pal[:,:,3]=255
    four=(c0>c1)[:,None]
    pal[:,2,:3]=np.where(four,(2*p0+p1)//3,(p0+p1)//2)
    pal[:,3,:3]=np.where(four,(p0+2*p1)//3,0)
    pal[:,3,3]=np.where(four[:,0],255,0)
    bits=cb[:,4].astype(np.uint32)|(cb[:,5].astype(np.uint32)<<8)|(cb[:,6].astype(np.uint32)<<16)|(cb[:,7].astype(np.uint32)<<24)
    sh=(np.arange(16)*2)[None,:]
    sel=((bits[:,None]>>sh)&3)
    px=np.take_along_axis(pal,sel[:,:,None],1)  # n,16,4
    if dxt5:
        a0=arr[:,0].astype(np.int32); a1=arr[:,1].astype(np.int32)
        ab=np.zeros(len(a0),np.uint64)
        for i in range(6): ab|=arr[:,2+i].astype(np.uint64)<<np.uint64(8*i)
        al=np.zeros((len(a0),8),np.int32); al[:,0]=a0; al[:,1]=a1
        gt=(a0>a1)
        for i in range(1,7):
            al[:,i+1]=np.where(gt,((7-i)*a0+i*a1)//7,0)
        for i in range(1,5):
            al[:,i+1]=np.where(gt,al[:,i+1],((5-i)*a0+i*a1)//5)
        al[:,6]=np.where(gt,al[:,6],0); al[:,7]=np.where(gt,al[:,7],255)
        asel=((ab[:,None]>>(np.arange(16,dtype=np.uint64)*np.uint64(3))[None,:])&np.uint64(7)).astype(np.int64)
        px[:,:,3]=np.take_along_axis(al,asel,1)
    img=px.reshape(bh,bw,4,4,4).transpose(0,2,1,3,4).reshape(bh*4,bw*4,4)
    return img[:h,:w].astype(np.uint8)
