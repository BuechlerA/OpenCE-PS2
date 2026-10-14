import struct, numpy as np
from xmap import load
from anim import graph, B, blk
def quat_rows(q):
    i,j,k,w=q; x,y,z=2*i,2*j,2*k
    xx,xy,xz=i*x,i*y,i*z; yy,yz,zz=j*y,j*z,k*z; wx,wy,wz=w*x,w*y,w*z
    return np.array([[1-(yy+zz), xy-wz, xz+wy],
                     [xy+wz, 1-(xx+zz), yz-wx],
                     [xz-wy, yz+wx, 1-(xx+yy)]])
def mat4(R,t):
    M=np.eye(4); M[:3,:3]=R; M[3,:3]=t; return M
def model_nodes(td,model_addr):
    mo=model_addr-B
    c,a=blk(td,mo+0xac+12)
    nodes=[]
    for i in range(c):
        o=a-B+i*0x9c
        parent=struct.unpack_from('<h',td,o+0x24)[0]
        t=struct.unpack_from('<3f',td,o+0x28); q=struct.unpack_from('<4f',td,o+0x34)
        nodes.append(dict(parent=parent,t=t,q=q))
    return nodes
def world(nodes,locals_):
    W=[None]*len(nodes)
    done=False
    for i,n in enumerate(nodes):
        p=n['parent']
        assert p<i
        W[i]=locals_[i]@(W[p] if p>=0 else np.eye(4))
    return W
def bind_world(nodes):
    return world(nodes,[mat4(quat_rows(n['q']),n['t']) for n in nodes])
def eval_anim(td,a,frame,count=19):
    """orientations (rotation quaternion, translation) of every node at an uncompressed frame"""
    assert not (a['flags']&1)
    fs=a['fs']; fdata=a['data'][1]-B+frame*fs; ddata=a['default'][1]-B
    out=[]; p=fdata; dp=ddata
    for n in range(count):
        li=n>>5
        rf=(a['rflags'][li]>>(n&31))&1; tf=(a['tflags'][li]>>(n&31))&1; sf=(a['sflags'][li]>>(n&31))&1
        if rf: q=struct.unpack_from('<4h',td,p); p+=8
        else: q=struct.unpack_from('<4h',td,dp); dp+=8
        if tf: t=struct.unpack_from('<3f',td,p); p+=12
        else: t=struct.unpack_from('<3f',td,dp); dp+=12
        if sf: s=struct.unpack_from('<f',td,p)[0]; p+=4
        else: s=struct.unpack_from('<f',td,dp)[0]; dp+=4
        q=np.array(q,float)/32767.0
        q/=np.linalg.norm(q)
        out.append((q,np.array(t),s))
    assert p-fdata==fs,(p-fdata,fs)
    return out
if __name__=='__main__':
    full,off,size,td,h=load('/Users/alex/Developer/OpenCE/assets/maps/bloodgulch.map')
    nodes,anims=graph(td,0x8072bd4c)
    mn=model_nodes(td,0x80729b64)
    print([n['parent'] for n in mn])
    a=anims[192]
    print(a['name'],a['fc'],a['fs'],a['finfo'],a['fit'])
    o=eval_anim(td,a,0)
    for n in range(3): print(o[n], mn[n]['t'],mn[n]['q'])
