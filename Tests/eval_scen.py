import numpy as np, json, subprocess, sys
import gen_scen
FS=44100
def run(name, exe, group, stab=1, atrim=1, trim_db=0.0, host=0.0, seed=3, quiet=False):
    cfg=gen_scen.make(name, seed); meta=json.load(open("/tmp/sc_meta.json")); N=meta["N"]; F0=meta["F0"]; pk0=meta["pk0"]
    args=[exe,"/tmp/sc_out.f32",str(int(group)),"55",str(FS),str(stab),str(atrim),str(trim_db)]+([str(host)] if host else [])
    out=subprocess.run(args,capture_output=True,text=True).stdout; L=int(out.split()[1])
    y=np.fromfile("/tmp/sc_out.f32",dtype=np.float32).astype(float)
    if group:                                       # isolate the bass: remove the (delayed) kick that was in the main input
        K=np.fromfile("/tmp/sc_side.f32",dtype=np.float32).astype(float)/cfg["sc_gain"]*cfg["main_kick_gain"]
        y=y-np.concatenate([np.zeros(L),K])[:len(y)]
    wrap=lambda d:(d+180)%360-180
    def phase_at(start):
        i=np.arange(start,start+N); return np.angle(np.sum(y[start:start+N]*np.exp(-2j*np.pi*F0*i/FS)))
    res={"start":[],"mid":[]}
    for nt in meta["notes"]:
        if nt["b"]<10: continue
        kick0=pk0-np.pi/2-2*np.pi*F0*nt["t0"]/FS
        for key,off in (("start",0.001),("mid",0.022)):
            s=nt["a"]+L+int(off*FS)
            if s+N>=len(y): continue
            d=np.degrees(phase_at(s)+2*np.pi*F0*L/FS-kick0)
            res[key].append(wrap(d))
    if not quiet:
        print(f"[{name}] LAT {L}  engine={exe.split('_')[-1]}  stab={stab} autoTrim={atrim}")
        for key in ("start","mid"):
            v=np.array(res[key]); e=np.abs(v)
            print(f"   {key:5s}: n={len(v):3d} bias {np.mean(v):+6.1f}  std {np.std(v):5.1f}  mean|e| {np.mean(e):5.1f}  p95 {np.percentile(e,95):5.1f}  max {np.max(e):5.1f}")
    return res,meta
def rhythm_report(meta):
    lg=np.loadtxt("/tmp/sc_log.txt"); beat=60/175.0
    labels=meta["labels"]; ok=tot=0; wrong=[]
    for b in range(8,len(labels)-2):
        t=int((b+1)*beat*FS+0.035*FS); row=lg[np.searchsorted(lg[:,0],t)-1]
        st=int(row[1]); got={1:'S',2:'T',3:'F'}.get(st,'?'); L=labels[b]; tot+=1
        if got==L: ok+=1
        else: wrong.append((b,L,got))
    last=lg[-1]
    print(f"   rhythm: {ok}/{tot} beats classified ok   final: 16th% {int(last[2])} density {int(last[3])}% onGrid {int(last[4])}% bpm {last[6]:.1f} trim {last[7]:+.1f} dB   mismatches {wrong[:6]}")

def report():
    import os
    v1=os.environ.get("SCEN_V1","/tmp/scen_v1"); v2=os.environ.get("SCEN_V2","/tmp/scen_v2")
    import numpy as np
    def line(res):
        out=[]
        for k in ("start","mid"):
            v=np.abs(np.array(res[k])); out.append(f"{k}: mean {np.mean(v):4.1f}  p95 {np.percentile(v,95):5.1f}  max {np.max(v):5.1f}")
        return " | ".join(out)
    print("Error in degrees of the bass-vs-kick phase, per note ('start' = first 20 ms of the note, includes the all-pass settling; 'mid' = steady part)")
    rows=(("free",0),("free_detune",0),("keysync",0),("longkick",0),("noisy",0),("together",0),("mixed",0),("group_ok",1),("group_mismatch",1),("group_hats",1))
    for name,g in rows:
        r1,_=run(name,v1,g,quiet=True,atrim=0); r3,meta=run(name,v2,g,quiet=True)
        print(f"{name:15s} old: {line(r1)}"); print(f"{'':15s} new: {line(r3)}")
    print()
    for name,g in (("mixed",0),("noisy",0),("group_hats",1)):
        r,meta=run(name,v2,g,quiet=True); print(f"{name}:",end=""); rhythm_report(meta)
if __name__=="__main__": report()
