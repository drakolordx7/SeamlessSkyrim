import os, sys, traceback
OUT = os.environ.get("RDC_OUT", os.getcwd())
CAP = os.environ["RDC_CAPTURE"]
log = open(os.path.join(OUT, "survey.txt"), "w")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, '', None)
    p("open", res)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    p("replay", res)
    sf = ctl.GetStructuredFile()
    texs = {t.resourceId: t for t in ctl.GetTextures()}
    counts = {}
    order = []
    def walk(acts, depth=0, path=""):
        for a in acts:
            name = a.GetName(sf)
            if a.children:
                walk(a.children, depth+1, path + "/" + name)
            if a.flags & rd.ActionFlags.Drawcall:
                key = (tuple(o for o in a.outputs if o != rd.ResourceId.Null()), a.depthOut)
                counts.setdefault(key, [0, a.eventId, a.eventId, path])
                c = counts[key]; c[0] += 1; c[2] = a.eventId
    walk(ctl.GetRootActions())
    def tdesc(r):
        t = texs.get(r)
        return f"{int(r)}:{t.width}x{t.height}:{t.format.Name()}" if t else str(int(r))
    for key, (n, first, last, path) in sorted(counts.items(), key=lambda kv: kv[1][1]):
        if n >= 20:
            p(f"draws {n:6d} events {first}-{last} rt [{', '.join(tdesc(r) for r in key[0])}] depth {tdesc(key[1])} | {path[-120:]}")
    # Thumbnails of the render targets with the most draws, and of the swapchain-sized ones.
    big = sorted(counts.items(), key=lambda kv: -kv[1][0])[:6]
    saved = set()
    for key, (n, first, last, path) in big:
        for r in key[0]:
            if r in saved:
                continue
            saved.add(r)
            ctl.SetFrameEvent(last, True)
            st = rd.TextureSave()
            st.resourceId = r
            st.destType = rd.FileType.PNG
            st.alpha = rd.AlphaMapping.Discard
            st.typeCast = rd.CompType.Typeless
            fn = os.path.join(OUT, f"rt_{int(r)}_ev{last}.png")
            p("save", fn, ctl.SaveTexture(st, fn))
    ctl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
p("done")
log.close()
os._exit(0)
