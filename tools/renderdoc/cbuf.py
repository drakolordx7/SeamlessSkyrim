import os, traceback
OUT = os.environ.get("RDC_OUT", os.getcwd())
CAP = os.environ["RDC_CAPTURE"]
log = open(os.path.join(OUT, "cbuf.txt"), "w")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()
def dump(vars, prefix=""):
    for v in vars:
        name = prefix + v.name
        if v.members:
            dump(v.members, name + ".")
        elif any(k in name for k in ("Seamless", "ExtraShaderDescriptor", "CameraPosAdjust", "PixelShaderDescriptor")):
            vals = [v.value.f32v[i] for i in range(min(4, v.columns * max(1, v.rows)))]
            ivals = [v.value.u32v[i] for i in range(min(4, v.columns * max(1, v.rows)))]
            p("    ", name, vals, ivals)
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    for e in (91153, 83339, 64535, 61177):
        ctl.SetFrameEvent(e, True)
        pipe = ctl.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
        p("event", e, "ps", refl.debugInfo.files[0].filename if refl and refl.debugInfo.files else "")
        if not refl:
            continue
        for i, cb in enumerate(refl.constantBlocks):
            bind = pipe.GetConstantBlock(rd.ShaderStage.Pixel, i, 0)
            vars = ctl.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Pixel, pipe.GetShaderEntryPoint(rd.ShaderStage.Pixel), i, bind.descriptor.resource, bind.descriptor.byteOffset, bind.descriptor.byteSize)
            p("  cb", i, cb.name)
            dump(vars)
    ctl.Shutdown(); cap.Shutdown()
except Exception:
    p(traceback.format_exc())
p("done"); log.close(); os._exit(0)
