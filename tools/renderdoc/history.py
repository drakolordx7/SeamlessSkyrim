import os, traceback
OUT = os.environ.get("RDC_OUT", os.getcwd())
CAP = os.environ["RDC_CAPTURE"]
log = open(os.path.join(OUT, "history.txt"), "w")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctl.GetStructuredFile()
    names = {}
    def walk(acts):
        for a in acts:
            names[a.eventId] = a.GetName(sf)
            if a.children: walk(a.children)
    walk(ctl.GetRootActions())
    # Draws in the depth pre-pass and the gbuffer pass whose mesh name hints at the street
    for lo, hi, label in ((60342, 99603, "depth prepass"), (99839, 172431, "gbuffer")):
        hits = [ (e, n) for e, n in names.items() if lo <= e <= hi and any(k in n for k in ("AlchemySign", "WRHouse", "WRWall", "WRMarket", "Landscape", "Land", "Belethor", "WRDragonDoor", "WRdragonDoor", "Mountain", "WRStone", "WR")) ]
        p(label, len(hits))
        for e, n in sorted(hits)[:60]:
            p("  ", e, n[-140:])
    tex = { int(t.resourceId): t for t in ctl.GetTextures() }
    def rid(n):
        for t in ctl.GetTextures():
            if int(t.resourceId) == n: return t.resourceId
    for target, label in ((349, "gbuffer color"), (498, "depth")):
        for (x, y) in ((1045, 470), (1045, 300), (1100, 600)):
            hist = ctl.PixelHistory(rid(target), x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            p(f"--- {label} pixel ({x},{y}): {len(hist)} modifications")
            for m in hist:
                flags = [f for f in ("shaderDiscarded", "depthTestFailed", "stencilTestFailed", "backfaceCulled", "scissorClipped", "viewClipped", "sampleMasked", "predicationSkipped", "unboundPS", "depthBoundsFailed") if getattr(m, f, False)]
                p(f"  ev {m.eventId} frag {m.fragIndex} prim {m.primitiveID} {flags} depth pre {m.preMod.depth:.6f} shader {m.shaderOut.depth:.6f} post {m.postMod.depth:.6f} | {names.get(m.eventId, '')[-110:]}")
    ctl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
p("done"); log.close(); os._exit(0)
