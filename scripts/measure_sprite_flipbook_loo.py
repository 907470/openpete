#!/usr/bin/env python3
"""Leave-one-out scorer for a 2D flipbook sprite: predicts cell k from cells k-1 and k+1 at alpha 0.5 by hold, cross-fade, an analytic vertical-axis rotation flow, and dense block-matching flow, and reports the mean abs error against the real cell k. Input: <scratch>/cells/cell_f*.ppm from OPENPETE_UISPRITE_CELLDUMP. Usage: measure_sprite_flipbook_loo.py <scratch>

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import glob,re,math,sys
import numpy as np
from PIL import Image
S=sys.argv[1]
fs=sorted(glob.glob(S+"/cells/cell_f*.ppm"), key=lambda f:int(re.search(r'_f(\d+)_',f).group(1)))[:9]
cells=[np.asarray(Image.open(f).convert('RGB')).astype(float) for f in fs]
H,W,_=cells[0].shape; print("cells",len(cells),"size",W,"x",H)
# tile for viewing
k=10; out=Image.new('RGB',(W*k*9+8*9,H*k),(40,40,40))
for i,c in enumerate(cells): out.paste(Image.fromarray(c.astype(np.uint8)).resize((W*k,H*k),Image.NEAREST),(i*(W*k+8),0))
out.save(S+"/egg_cells.png")
mask=[(c.sum(2)>0) for c in cells]
union=np.logical_or.reduce(mask); print("silhouette identical across cells:", all((m==union).all() for m in mask), "px",union.sum())
# per-row half-width + centre of the silhouette (ellipse fit by rows)
rows={}
for y in range(H):
    xs=np.where(union[y])[0]
    if len(xs): rows[y]=((xs[0]+xs[-1]+1)/2.0, (xs[-1]+1-xs[0])/2.0)
print("row centre/halfwidth:", {y:(round(c,1),hw) for y,(c,hw) in rows.items()})
# spot tracking: dark pixels (purple spots) centroid per frame
def spots(c):
    lum=c.mean(2); m=(lum>0)&(lum<c.max()*0.6)
    return m
for i,c in enumerate(cells):
    m=spots(c); ys,xs=np.where(m)
    print(f"frame {i}: spot px={m.sum()} x-centroid={xs.mean() if len(xs) else float('nan'):.2f}")
# --- predictors for cell k from k-1 and k+1 at alpha .5 ---
def err(pred,truth): 
    m=union; return np.abs(pred-truth)[m].mean()
def hold(a,b): return a.copy()
def xfade(a,b): return 0.5*a+0.5*b
def sample(img,x,y):
    # bilinear horizontal sample at real x, row y, within silhouette row; outside -> nan
    x0=int(math.floor(x)); t=x-x0
    def px(xx): 
        return img[y,xx] if 0<=xx<W and union[y,xx] else None
    p0,p1=px(x0),px(x0+1)
    if p0 is None and p1 is None: return None
    if p0 is None: return p1
    if p1 is None: return p0
    return (1-t)*p0+t*p1
def flow_warp(a,b,theta,sign):
    """ellipsoid rotating about vertical axis by theta per tick; predict midpoint."""
    pred=np.zeros_like(a)
    for y in range(H):
        if y not in rows: continue
        cx,hw=rows[y]
        for x in range(W):
            if not union[y,x]: continue
            u=(x+0.5-cx)/hw; u=max(-1,min(1,u)); phi=math.asin(u)
            # midpoint pixel came from a at phi - sign*theta/2, will go to b at phi + sign*theta/2
            sa=phi-sign*theta/2; sb=phi+sign*theta/2
            pa = sample(a, cx+hw*math.sin(sa)-0.5, y) if abs(sa)<=math.pi/2 else None
            pb = sample(b, cx+hw*math.sin(sb)-0.5, y) if abs(sb)<=math.pi/2 else None
            if pa is None and pb is None: pred[y,x]=0.5*(a[y,x]+b[y,x])
            elif pa is None: pred[y,x]=pb
            elif pb is None: pred[y,x]=pa
            else: pred[y,x]=0.5*(pa+pb)
    return pred
n=len(cells)
def score(fn,name):
    e=[err(fn(cells[(i-1)%n],cells[(i+1)%n]),cells[i]) for i in range(n)]
    print(f"{name:28s} mean err {np.mean(e):6.2f}  per-frame {[round(v,1) for v in e]}")
    return np.mean(e)
score(hold,"hold (no interp)"); score(xfade,"cross-fade")
best=None
for sign in (+1,-1):
    for deg in (10,15,20,25,30,35,40,45,50,60):
        th=math.radians(deg)
        e=score(lambda a,b,th=th,s=sign: flow_warp(a,b,th,s), f"flow sign={sign:+d} {deg}deg/tick")
        if best is None or e<best[0]: best=(e,sign,deg)
print("BEST analytic flow:",best)

# --- baked flow: dense block matching a->b, symmetric half-warp ---
def bm_flow(a,b,R=4,P=1):
    Hh,Ww,_=a.shape; flow=np.zeros((Hh,Ww,2))
    pa=np.pad(a,((P+R,P+R),(P+R,P+R),(0,0))); pb=np.pad(b,((P+R,P+R),(P+R,P+R),(0,0)))
    for y in range(Hh):
        for x in range(Ww):
            if not union[y,x]: continue
            best=None
            A=pa[y+R:y+R+2*P+1, x+R:x+R+2*P+1]
            for dy in range(-R,R+1):
                for dx in range(-R,R+1):
                    B=pb[y+R+dy:y+R+dy+2*P+1, x+R+dx:x+R+dx+2*P+1]
                    d=np.abs(A-B).sum()+0.5*(abs(dx)+abs(dy))   # small smoothness prior
                    if best is None or d<best[0]: best=(d,dx,dy)
            flow[y,x]=(best[1],best[2])
    return flow
def warp(img,flow,s):
    out=np.zeros_like(img); cnt=np.zeros(img.shape[:2])
    Hh,Ww,_=img.shape
    for y in range(Hh):
        for x in range(Ww):
            if not union[y,x]: continue
            tx=x+flow[y,x,0]*s; ty=y+flow[y,x,1]*s
            xi=int(round(tx)); yi=int(round(ty))
            if 0<=xi<Ww and 0<=yi<Hh: out[yi,xi]+=img[y,x]; cnt[yi,xi]+=1
    m=cnt>0; out[m]/=cnt[m][:,None]; return out,m
def baked(a,b):
    fab=bm_flow(a,b); fba=bm_flow(b,a)
    wa,ma=warp(a,fab,0.5); wb,mb=warp(b,fba,0.5)
    pred=np.zeros_like(a)
    both=ma&mb; pred[both]=0.5*(wa[both]+wb[both])
    pred[ma&~mb]=wa[ma&~mb]; pred[mb&~ma]=wb[mb&~ma]
    hole=~(ma|mb)&union; pred[hole]=0.5*(a[hole]+b[hole])
    return pred
score(baked,"baked flow (block match)")
# upper bound sanity: what if we had the frames at 2x rate? n/a. Save a visual of predictions for frame 4
i=4; a,b,t=cells[(i-1)%n],cells[(i+1)%n],cells[i]
preds=[("prev",a),("truth",t),("next",b),("xfade",xfade(a,b)),("flow-vert",flow_warp(a,b,math.radians(15),-1)),("baked",baked(a,b))]
k=10; out=Image.new('RGB',(len(preds)*(W*k+8),H*k),(40,40,40))
for j,(nm,p) in enumerate(preds): out.paste(Image.fromarray(np.clip(p,0,255).astype(np.uint8)).resize((W*k,H*k),Image.NEAREST),(j*(W*k+8),0))
out.save(S+"/egg_pred.png"); print("saved", [p[0] for p in preds])
