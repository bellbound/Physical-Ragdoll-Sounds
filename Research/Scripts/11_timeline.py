"""Rate of events against frame rate, and what happens after the ragdoll ends."""
import numpy as np, pandas as pd, rlib
ep = pd.read_csv("episodes.csv")

print(f"{'take':<30} {'fps':>5} {'rag_s':>6} {'imp/s':>7} {'eps/s':>7} {'rows/ep':>8}")
pts = []
for t in rlib.takes():
    df = t.load()
    ts = np.sort(df[df["event"].isin(["impact","touch","separate"])]["t_ms"].to_numpy())
    d = np.diff(ts); fps = 1000/np.median(d[d > 2.0])
    st = df[(df["event"]=="state")]
    starts = st[st["state"]=="ragdoll_start"]["t_ms"].to_numpy()
    ends = st[st["state"]=="ragdoll_end"]["t_ms"].to_numpy()
    win = []
    for s in starts:
        e2 = ends[ends > s]
        win.append((s, e2[0] if len(e2) else df["t_ms"].max()))
    rag = sum(b-a for a, b in win)/1000
    if rag <= 0:
        # never ragdolled at all - the standing actor in stack-onto-standing
        print(f"{t.stem:<30} {fps:>5.1f}   never ragdolled, "
              f"{len(df[df['event']=='impact'])} impacts")
        continue
    imp = df[df["event"]=="impact"]
    e = ep[ep["take"]==t.stem]
    print(f"{t.stem:<30} {fps:>5.1f} {rag:>6.1f} {len(imp)/rag:>7.1f} {len(e)/rag:>7.1f} "
          f"{len(imp)/max(len(e),1):>8.2f}")
    pts.append((fps, len(imp)/rag, len(e)/rag))
p = np.array(pts)
print(f"\nspearman(fps, impact rows/s)  = {pd.Series(p[:,0]).rank().corr(pd.Series(p[:,1]).rank()):+.2f}")
print(f"spearman(fps, episodes/s)     = {pd.Series(p[:,0]).rank().corr(pd.Series(p[:,2]).rank()):+.2f}")

# There is no ragdoll_end anywhere in this dataset: the scripted run paralyses
# the subject, so every take closes with the actor still limp. Nothing below
# fires, and that is the finding - see Research/04-Findings.md.
print("\n=== what happens outside the ragdoll window ===")
for t in rlib.takes():
    df = t.load()
    st = df[df["event"]=="state"]
    starts = st[st["state"]=="ragdoll_start"]["t_ms"].to_numpy()
    ends = st[st["state"]=="ragdoll_end"]["t_ms"].to_numpy()
    imp = df[df["event"]=="impact"]
    inside = np.zeros(len(imp), bool)
    v = imp["t_ms"].to_numpy()
    if not len(v):
        print(f"{t.stem:<30} no impacts")
        continue
    for s in starts:
        e2 = ends[ends > s]
        e2 = e2[0] if len(e2) else v.max()+1
        inside |= (v >= s) & (v <= e2)
    getup = st[st["state"]=="knock_get_up"]["t_ms"].to_numpy()
    after = 0
    if len(getup):
        after = int((v > getup[0]).sum())
    print(f"{t.stem:<30} impacts inside ragdoll={inside.sum():>5}  outside={int((~inside).sum()):>4}"
          f"  after first knock_get_up={after:>4}")
