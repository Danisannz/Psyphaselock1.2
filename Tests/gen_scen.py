# Scenario generator: writes /tmp/sc_main.f32 /tmp/sc_side.f32 /tmp/sc_meta.json
import numpy as np, json, sys
FS=44100; BPM=175.0; N=int(round(FS/55.0)); F0=FS/N; beat=60/BPM; sixt=beat/4
def make(name, seed=3):
    r=np.random.default_rng(seed)
    cfg=dict(bass_len=0.075,jitter=0.0004,glitch=0.0,detune=0.0,noise_db=-60,keysync=False,kick_len=0.2,group=False,sc_gain=1.0,main_kick_gain=1.0,beats=64,pattern="off",fills=False,hats=False)
    cfg.update(dict(
        free={}, free_detune=dict(detune=0.25), keysync=dict(keysync=True,noise_db=-45), longkick=dict(kick_len=0.3),
        mixed=dict(fills=True), group_ok=dict(group=True), group_mismatch=dict(group=True,sc_gain=0.8),
        group_hats=dict(group=True,hats=True,sc_gain=0.9,fills=True), glitchy=dict(keysync=True,glitch=0.2), glitchy_hard=dict(keysync=True,glitch=0.2,glitch_amp=2.0,jitter=0.0), short_key=dict(keysync=True,bass_len=0.030,jitter=0.0), short_free=dict(bass_len=0.030,jitter=0.0), key_quant=dict(keysync=True,jitter=0.0), together=dict(pattern="together",group=False), noisy=dict(noise_db=-38,detune=0.15,fills=True),
    )[name])
    n=int((cfg["beats"]+1)*beat*FS); K=np.zeros(n); B=np.zeros(n)
    # kick (tuned to F0, exponential pitch sweep, saturation, gated tail)
    Lk=cfg["kick_len"]; m=int(Lk*FS); t=np.arange(m)/FS
    f=F0+(250-F0)*np.exp(-t/0.012); y=np.sin(2*np.pi*np.cumsum(f)/FS)*np.exp(-t/0.08)*np.clip((Lk-t)/0.025,0,1)
    y=np.tanh(1.8*y)/np.tanh(1.8); k1=0.8*y/np.abs(y).max(); pk0=2*np.pi*(250-F0)*0.012
    notes=[]; labels=[]
    for b in range(cfg["beats"]):
        t0=int(b*beat*FS); K[t0:t0+len(k1)]+=k1[:n-t0]
        lab='S'; slots=[1,2,3] if cfg["pattern"]=="off" else [0,1,2,3]
        if cfg["fills"] and b in (22,23): lab='F'; slots=[0.5,1.5,2.0,2.5,3.5]
        if cfg["fills"] and b==34: lab='T'; slots=[1,3]
        if cfg["fills"] and b==44: lab='T'; slots=[1,2,3.4]
        labels.append(lab)
        for sl in slots:
            a=int((b*beat+sl*sixt+r.normal(0,cfg['jitter']))*FS); ln=int((cfg['bass_len'] if lab!='F' else 0.03)*FS); tt=np.arange(ln)/FS
            fr=F0+cfg["detune"]; evaluable=(lab!='F')
            if lab=='F': fr=F0*1.12                                  # fills change pitch
            ph=(0.7+0.4*sl if cfg["keysync"] else r.uniform(0,2*np.pi)); amp=r.uniform(0.7,1.0)
            env=np.minimum(1,np.minimum(tt/0.003,(ln/FS-tt)/0.003))
            yy=(0.5*np.cos(2*np.pi*fr*tt+ph)+0.2*np.cos(2*np.pi*2*fr*tt+2*ph+1)+0.1*np.cos(2*np.pi*3*fr*tt+3*ph+2))*env*amp
            glitched = (cfg["glitch"]>0 and lab=='S' and r.random()<cfg["glitch"] and b>=12)
            if glitched:                                   # a burst of broadband noise over the first 25 ms of the note
                gl=int(0.025*FS); yy[:gl]+=r.standard_normal(gl)*cfg.get("glitch_amp",0.35)
            B[a:a+ln]+=yy[:n-a]
            if evaluable: notes.append(dict(a=a,t0=t0,b=b,slot=float(sl),ev=True,glitched=bool(glitched),truth=float(np.degrees(ph-2*np.pi*F0*((a-t0)/FS)-pk0)+90)))
    noise=lambda: r.standard_normal(n)*10**(cfg["noise_db"]/20)*0.05
    B+=noise(); K+=noise()*0
    side=K*cfg["sc_gain"]
    mainsig=B.copy()
    if cfg["group"]: mainsig=B+K*cfg["main_kick_gain"]
    if cfg["hats"]:
        h=r.standard_normal(n)*0.05
        for b in range(cfg["beats"]*2):
            a=int((b*beat/2+beat/4)*FS); hl=int(0.03*FS); h[a:a+hl]*=8*np.exp(-np.arange(hl)/FS/0.006)
        mainsig=mainsig+h*0.3
    mainsig.astype(np.float32).tofile("/tmp/sc_main.f32"); side.astype(np.float32).tofile("/tmp/sc_side.f32")
    json.dump(dict(cfg=cfg,pk0=pk0,notes=notes,labels=labels,N=N,F0=F0),open("/tmp/sc_meta.json","w"))
    return cfg
if __name__=="__main__": make(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 3)
