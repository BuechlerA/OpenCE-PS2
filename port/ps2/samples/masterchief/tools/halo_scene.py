import sys, math, struct, numpy as np
from PIL import Image
from mesh import get_mesh, decode_vec
from xmap import load
from dxt import decode_dxt1
from bmp import bitmap_info
from skel import *
from anim import graph, B

import os
MAP=os.environ.get('HALO_MAP',os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','..','..','..','assets','maps','bloodgulch.map'))
full,_off,_size,td,_h=load(MAP)
CHIEF=0x80729b64; CHIEF_ANTR=0x8072bd4c; AR_MODEL=0x8080e70c

# ---- tag lookup ----
TAGS={}
for _i in range(_h[2]):
    _o=(_h[0]-B)+_i*0x20
    _g=td[_o:_o+4][::-1]; _idx,_nm,_ba=struct.unpack_from('<III',td,_o+0xc)
    TAGS[(_g,td[_nm-B:td.index(b'\0',_nm-B)].decode())]=_ba
def tagref(o):
    g,nm,ln,idx=struct.unpack_from('<4sIII',td,o)
    return td[nm-B:td.index(b'\0',nm-B)].decode() if nm else None

# ---- bitmaps (all DXT1 here; pixels sit at the bitmap's pixels_offset in the map file) ----
_cache={}
def bitmap(name):
    if name in _cache: return _cache[name]
    b=bitmap_info(td,TAGS[(b'bitm',name)])[0]
    assert b['fmt']==14,(name,b)
    w,h=b['w'],b['h']
    if b['type']==2:   # cube map: six faces, 2816 bytes apart for 64x64 with mips
        stride=b['pixsize']//6
        r=np.stack([decode_dxt1(full[b['pixoff']+f*stride:b['pixoff']+f*stride+w*h//2],w,h)[...,:3].astype(np.float64)/255 for f in range(6)])
    else:
        r=decode_dxt1(full[b['pixoff']:b['pixoff']+w*h//2],w,h)[...,:3].astype(np.float64)/255
    _cache[name]=r; return r

def soso(name,**kw):
    so=TAGS[(b'soso',name)]-B
    m=dict(base=bitmap(tagref(so+0xA4)),multi=bitmap(tagref(so+0xBC)),detail=None,dscale=1.0,cube=None,
           uv=struct.unpack_from('<2f',td,so+0x9c),cc=False,visor=False,perp=np.ones(3),par=np.ones(3))
    d=tagref(so+0xDC)
    if d: m['detail']=bitmap(d); m['dscale']=struct.unpack_from('<f',td,so+0xD8)[0]
    c=tagref(so+0x164)
    if c:
        m['cube']=bitmap(c); rf=struct.unpack_from('<10f',td,so+0x13c)
        m['perp']=np.array(rf[3:6])*rf[2]; m['par']=np.array(rf[7:10])*rf[6]
    m.update(kw); return m
IK=True
CHANGE=np.array([0.341176,0.403922,0.145098])      # the biped's primary change colour
MATS=[]
def material(name,**kw):
    MATS.append(soso(name,**kw)); return len(MATS)-1

def parts_of(addr,geom,mats,nnodes,skinned):
    out=[]
    for p in get_mesh(MAP,addr,geom):
        nm=p['shader']
        if nm not in mats: continue
        v=p['verts']
        P=np.array([a[:3] for a in v],dtype=np.float64)
        N=np.array([decode_vec(a[3]) for a in v]); N/=np.maximum(np.linalg.norm(N,axis=1,keepdims=True),1e-9)
        UV=np.array([(a[6]/32767.0, a[7]/32767.0) for a in v])
        NODES=np.array([(a[8]//3,a[9]//3) for a in v]); WGT=np.array([a[10]/32767.0 for a in v])
        bad=NODES[:,1]>=nnodes; NODES[bad,1]=NODES[bad,0]; WGT[bad]=1.0
        NODES[NODES[:,0]>=nnodes,0]=0
        idx=p['idx']; tris=[]
        for i in range(len(idx)-2):
            a,b,c=idx[i],idx[i+1],idx[i+2]
            if a==b or b==c or a==c: continue
            tris.append((a,c,b) if i&1 else (a,b,c))
        out.append(dict(P=P,N=N,UV=UV,T=np.array(tris),NODES=NODES,WGT=WGT,mat=mats[nm],skinned=skinned))
    return out

M_ARMOR=material('characters\\cyborg\\shaders\\armor',cc=True)
M_VISOR=material('characters\\cyborg\\shaders\\visor',visor=True)
chief=parts_of(CHIEF,0,{'characters\\cyborg\\shaders\\armor':M_ARMOR,'characters\\cyborg\\shaders\\visor':M_VISOR},19,True)
M_GRIP=material('weapons\\assault rifle\\fp\\shaders\\grip')
M_GUN=material('weapons\\assault rifle\\fp\\shaders\\gun')
M_DISP=material('weapons\\assault rifle\\fp\\shaders\\display')
rifle=parts_of(AR_MODEL,0,{'weapons\\assault rifle\\fp\\shaders\\grip':M_GRIP,'weapons\\assault rifle\\fp\\shaders\\gun':M_GUN,'weapons\\assault rifle\\fp\\shaders\\display':M_DISP},3,False)

allp=np.concatenate([m['P'] for m in chief]); lo=allp.min(0); hi=allp.max(0)
center=(lo+hi)/2; radius=np.linalg.norm(hi-lo)/2

# ---- skeleton and animation ----
_gn,_anims=graph(td,CHIEF_ANTR)
MN=model_nodes(td,CHIEF)
INVBIND=[np.linalg.inv(m) for m in bind_world(MN)]
ANIM=_anims[172]       # "stand rifle move-front"
FINFO=np.array(struct.unpack_from('<%df'%(ANIM['fc']*2),td,ANIM['finfo'][1]-B)).reshape(-1,2)
STRIDE=FINFO[:,0].sum()
def marker(model,name,node_hint=None):
    mo=model-B
    c,a,_=struct.unpack_from('<III',td,mo+0xac)
    for i in range(c):
        e=a-B+i*0x40
        if td[e:e+32].split(b'\0')[0].decode()==name:
            ic,ia,_=struct.unpack_from('<III',td,e+0x34)
            q=ia-B
            node=struct.unpack_from('<B',td,q+2)[0]
            return node,mat4(quat_rows(struct.unpack_from('<4f',td,q+0x10)),struct.unpack_from('<3f',td,q+4))
RH_NODE,RH=marker(CHIEF,'right hand')
LH_NODE,LH=marker(CHIEF,'left hand')
_,AR_LH=marker(AR_MODEL,'left hand cyborg')
def pose(t):
    f=(t%1.0)*ANIM['fc']; f0=int(f)%ANIM['fc']; f1=(f0+1)%ANIM['fc']; k=f-int(f)
    o0=eval_anim(td,ANIM,f0); o1=eval_anim(td,ANIM,f1)
    L=[]
    for (q0,t0,_),(q1,t1,_) in zip(o0,o1):
        if np.dot(q0,q1)<0: q1=-q1
        q=q0*(1-k)+q1*k; q/=np.linalg.norm(q)
        L.append(mat4(quat_rows(q),t0*(1-k)+t1*k))
    Wd=world(MN,L)
    return [INVBIND[i]@Wd[i] for i in range(len(MN))],Wd
def skin(P,N,NODES,WGT,S):
    Ph=np.concatenate([P,np.ones((len(P),1))],1)
    M0=np.stack([S[i] for i in range(len(S))])
    A=np.einsum('vi,vij->vj',Ph,M0[NODES[:,0]]); B_=np.einsum('vi,vij->vj',Ph,M0[NODES[:,1]])
    pos=A[:,:3]*WGT[:,None]+B_[:,:3]*(1-WGT[:,None])
    n0=np.einsum('vi,vij->vj',N,M0[NODES[:,0],:3,:3]); n1=np.einsum('vi,vij->vj',N,M0[NODES[:,1],:3,:3])
    nn=n0*WGT[:,None]+n1*(1-WGT[:,None]); nn/=np.maximum(np.linalg.norm(nn,axis=1,keepdims=True),1e-9)
    return pos,nn
def place(P,N,M):
    return (np.concatenate([P,np.ones((len(P),1))],1)@M)[:,:3], N@M[:3,:3]

def sample(t,u,v):
    h,w=t.shape[:2]
    x=u*w-0.5; y=v*h-0.5
    x0=np.floor(x).astype(int); y0=np.floor(y).astype(int); fx=(x-x0)[...,None]; fy=(y-y0)[...,None]
    g=lambda xx,yy: t[yy%h,xx%w]
    return (g(x0,y0)*(1-fx)*(1-fy)+g(x0+1,y0)*fx*(1-fy)+g(x0,y0+1)*(1-fx)*fy+g(x0+1,y0+1)*fx*fy)
def sample_cube(faces,d):
    d=d/np.maximum(np.linalg.norm(d,axis=1,keepdims=True),1e-9)
    ax=np.abs(d); m=ax.argmax(1); n=len(d); out=np.zeros((n,3)); res=faces.shape[1]
    for k in range(3):
        sel=m==k
        if not sel.any(): continue
        dd=d[sel]; a=ax[sel,k]
        pos=dd[:,k]>0
        o1,o2=[i for i in range(3) if i!=k]
        face=2*k+(~pos)
        u=0.5*(dd[:,o1]/a+1); v=0.5*(dd[:,o2]/a+1)
        px=np.clip((u*res).astype(int),0,res-1); py=np.clip((v*res).astype(int),0,res-1)
        out[sel]=faces[face,py,px]
    return out
def norm(v): return v/np.linalg.norm(v)

def rot_between(a,b):
    a=a/np.linalg.norm(a); b=b/np.linalg.norm(b)
    v=np.cross(a,b); c=np.dot(a,b); s_=np.linalg.norm(v)
    if s_<1e-9: return np.eye(3)
    k=v/s_; K=np.array([[0,-k[2],k[1]],[k[2],0,-k[0]],[-k[1],k[0],0]])
    return np.eye(3)+K*s_+K@K*(1-c)
def about(R,pivot):
    # row-vector 4x4 rotating points about pivot by column-convention R
    M=np.eye(4); M[:3,:3]=R.T; M[3,:3]=pivot-pivot@R.T; return M
def left_arm_ik(Wd,target_m,target_rot,blend_hand=1.0):
    Wd=list(Wd)
    s_=Wd[13][3,:3]; e=Wd[15][3,:3]; m=(LH@Wd[LH_NODE])[3,:3]
    t=target_m
    l1=np.linalg.norm(e-s_); l2=np.linalg.norm(m-e)
    d=np.linalg.norm(t-s_); d=min(max(d,abs(l1-l2)+1e-4),l1+l2-1e-4)
    a=(t-s_)/np.linalg.norm(t-s_); c_=(m-s_)/np.linalg.norm(m-s_)
    p=(e-s_)-np.dot(e-s_,c_)*c_
    q=p-np.dot(p,a)*a; q/=np.linalg.norm(q)
    x=(l1*l1-l2*l2+d*d)/(2*d); y=math.sqrt(max(l1*l1-x*x,0))
    e2=s_+a*x+q*y
    Q1=about(rot_between(e-s_,e2-s_),s_)
    for i in (13,15,17): Wd[i]=Wd[i]@Q1
    m1=(LH@Wd[LH_NODE])[3,:3]; e1=Wd[15][3,:3]
    Q2=about(rot_between(m1-e1,t-e1),e1)
    for i in (15,17): Wd[i]=Wd[i]@Q2
    # hand turns to the rifle's grip orientation
    cur=(LH@Wd[LH_NODE])[:3,:3]
    R=np.linalg.inv(cur)@target_rot          # row convention: cur @ R = target
    Q3=np.eye(4); Q3[:3,:3]=R; mt=(LH@Wd[LH_NODE])[3,:3]; Q3[3,:3]=mt-mt@R
    Wd[17]=Wd[17]@Q3
    return Wd
def scene(phase):
    S,Wd=pose(phase)
    Mw=RH@Wd[RH_NODE]
    if IK:
        tgt=AR_LH@Mw
        Wd=left_arm_ik(Wd,tgt[3,:3],tgt[:3,:3])
        S=[INVBIND[i]@Wd[i] for i in range(len(MN))]
    objs=[]
    for o in chief:
        P,N=skin(o['P'],o['N'],o['NODES'],o['WGT'],S); objs.append((P,N,o))
    for o in rifle:
        P,N=place(o['P'],o['N'],Mw); objs.append((P,N,o))
    return objs,Wd

def render(theta,phase=0.0,scroll=0.0,S=1600,out=800):
    objs,Wd=scene(phase)
    W=H=S
    c,s=math.cos(theta),math.sin(theta)
    R=np.array([[c,-s,0],[s,c,0],[0,0,1]])
    cam=center+np.array([radius*4.3,0,radius*0.55])
    fwd=norm(center-cam); right=norm(np.cross(fwd,[0,0,1])); up=np.cross(right,fwd)
    f=1.0/math.tan(math.radians(14))
    zbuf=np.full((H,W),np.inf)
    A=np.zeros((H,W,6)); COV=np.zeros((H,W),bool)
    for P,N,o in objs:
        UV,T=o['UV'],o['T']
        Pw=(P-center)@R.T+center; Nw=N@R.T
        rel=Pw-cam; xc=rel@right; yc=rel@up; zc=rel@fwd
        sx=(xc/zc*f*0.5+0.5)*W; sy=(0.5-yc/zc*f*0.5)*H
        att=np.concatenate([UV,Nw],1)
        for a,b,cc in T:
            x0,y0,x1,y1,x2,y2=sx[a],sy[a],sx[b],sy[b],sx[cc],sy[cc]
            area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0)
            if abs(area)<1e-9: continue
            minx=max(int(math.floor(min(x0,x1,x2))),0); maxx=min(int(math.ceil(max(x0,x1,x2))),W-1)
            miny=max(int(math.floor(min(y0,y1,y2))),0); maxy=min(int(math.ceil(max(y0,y1,y2))),H-1)
            if minx>maxx or miny>maxy: continue
            gx,gy=np.meshgrid(np.arange(minx,maxx+1)+0.5,np.arange(miny,maxy+1)+0.5)
            w0=((x1-gx)*(y2-gy)-(x2-gx)*(y1-gy))/area
            w1=((x2-gx)*(y0-gy)-(x0-gx)*(y2-gy))/area
            w2=1-w0-w1
            m=(w0>=-1e-6)&(w1>=-1e-6)&(w2>=-1e-6)
            if not m.any(): continue
            iz=w0/zc[a]+w1/zc[b]+w2/zc[cc]; z=1/iz
            sub=zbuf[miny:maxy+1,minx:maxx+1]
            m&=z<sub
            if not m.any(): continue
            sub[m]=z[m]
            pa=(w0[...,None]*att[a]/zc[a]+w1[...,None]*att[b]/zc[b]+w2[...,None]*att[cc]/zc[cc])*z[...,None]
            A[miny:maxy+1,minx:maxx+1][m]=np.concatenate([pa,np.full(pa.shape[:2]+(1,),float(o['mat']))],-1)[m]
            COV[miny:maxy+1,minx:maxx+1][m]=True
    # background and ground
    yy=np.linspace(0,1,H)[:,None,None]
    img=np.array([0.10,0.12,0.14])*(1-yy)+np.array([0.03,0.04,0.05])*yy
    img=img*np.ones((1,W,1))
    gz=lo[2]
    gy_,gx_=np.mgrid[0:H,0:W]
    ndcx=((gx_+0.5)/W-0.5)*2/f; ndcy=(0.5-(gy_+0.5)/H)*2/f
    rays=fwd[None,None]+right[None,None]*ndcx[...,None]+up[None,None]*ndcy[...,None]
    with np.errstate(divide='ignore',invalid='ignore'):
        tt=(gz-cam[2])/rays[...,2]
    hit=cam+rays*tt[...,None]
    ok=(tt>0)
    mp=(hit-center)@R+center
    gxm=mp[...,0]+scroll; gym=mp[...,1]
    cell=0.25
    def line(c,wd):
        d=np.abs((c/cell+0.5)%1.0-0.5)*cell
        return np.clip(1-d/wd,0,1)
    dist=np.hypot(mp[...,0]-center[0],mp[...,1]-center[1])
    fade=np.clip(1-dist/(radius*3.2),0,1)**1.5
    grid=np.maximum(line(gxm,0.003),line(gym,0.003))
    floorcol=np.array([0.075,0.09,0.11])+grid[...,None]*np.array([0.10,0.14,0.17])
    floorcol=floorcol+np.array([0.04,0.05,0.06])*np.clip(1-dist/(radius*1.6),0,1)[...,None]
    m=ok&(fade>0)
    img[m]=img[m]*(1-fade[m,None]*0.9)+floorcol[m]*fade[m,None]*0.9
    sh=np.exp(-((mp[...,0]-center[0])**2/(0.11**2)+(mp[...,1]-center[1])**2/(0.13**2)))
    img[ok]*=(1-0.55*sh[ok])[:,None]
    # deferred shading, material by material
    ys,xs=np.nonzero(COV)
    a=A[ys,xs]; matid=a[:,5].astype(int)
    u,v=a[:,0],a[:,1]
    nrm=a[:,2:5]; nrm/=np.maximum(np.linalg.norm(nrm,axis=1,keepdims=True),1e-9)
    ndcx=((xs+0.5)/W-0.5)*2/f; ndcy=(0.5-(ys+0.5)/H)*2/f
    ray=fwd[None]+right[None]*ndcx[:,None]+up[None]*ndcy[:,None]; ray/=np.linalg.norm(ray,axis=1,keepdims=True)
    V=-ray
    L=[(norm(np.array([0.6,-0.5,0.7])),np.array([1.0,0.95,0.85]),1.0),
       (norm(np.array([0.3,0.8,0.2])),np.array([0.45,0.55,0.8]),0.45),
       (norm(np.array([-0.8,-0.1,0.3])),np.array([0.9,0.95,1.0]),0.5)]
    diff=np.zeros((len(a),3))
    for d,lc,k in L: diff+=np.clip(nrm@d,0,1)[:,None]*lc*k
    Hh=V+L[0][0]; Hh/=np.linalg.norm(Hh,axis=1,keepdims=True)
    ndh=np.clip((nrm*Hh).sum(1),0,1)[:,None]
    ndv=np.clip((nrm*V).sum(1),0,1)[:,None]
    fres=1-ndv
    Rv=2*(nrm*V).sum(1,keepdims=True)*nrm-V
    Rc=np.stack([Rv[:,1],Rv[:,2],Rv[:,0]],1)
    col=np.zeros((len(a),3))
    for mi,mt in enumerate(MATS):
        sel=matid==mi
        if not sel.any(): continue
        uu=u[sel]*mt['uv'][0]; vv=v[sel]*mt['uv'][1]
        base=sample(mt['base'],uu,vv); mpt=sample(mt['multi'],uu,vv)
        alb=base
        if mt['detail'] is not None:
            det=sample(mt['detail'],uu*mt['dscale'],vv*mt['dscale'])
            alb=base*(0.65+0.7*det.mean(-1,keepdims=True))
        if mt['cc']:
            mask=mpt[:,2:3]; alb=alb*(1-mask)+alb*CHANGE*1.55*mask
        sm=mpt[:,0:1]; glow=mpt[:,1:2]
        vis=mt['visor']
        spec=ndh[sel]**(60 if vis else 18)*(0.15+1.1*sm)
        if mt['cube'] is not None:
            rc=sample_cube(mt['cube'],Rc[sel])
            if vis:
                refl=rc*(mt['perp']*(1-fres[sel])+mt['par']*fres[sel])*1.35
            else:
                refl=rc*(0.06+0.40*sm)*(0.5+fres[sel])
        else: refl=0
        col[sel]=alb*(diff[sel]+np.array([0.14,0.15,0.18]))+spec*np.array([1,0.97,0.9])+refl+glow*np.array([0.1,1.0,0.2])*0.8
    img[ys,xs]=col
    img=np.clip(img,0,1)**(1/2.2)
    return Image.fromarray((img*255).astype(np.uint8)).resize((out,out),Image.LANCZOS)
if __name__=='__main__':
    n=int(sys.argv[1]); outd=sys.argv[2]; cycles=int(sys.argv[3]) if len(sys.argv)>3 else 6
    for i in range(n):
        ph=cycles*i/n
        render(2*math.pi*i/n,phase=ph,scroll=-STRIDE*ph).save('%s/f%03d.png'%(outd,i)); print(i,end=' ',flush=True)
