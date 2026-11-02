#!/usr/bin/env python3
"""Exercise the production frame bridge using real, independent ANGLE contexts.

Requires a separately compiled host bridge library and official ANGLE libraries.
No APK, browser profile or iPad access. Hardware/backend is reported explicitly.
"""
import argparse
import ctypes as C
import faulthandler
import os
from pathlib import Path
import time

def main():
    faulthandler.enable()
    p=argparse.ArgumentParser();p.add_argument('angle',type=Path);p.add_argument('bridge',type=Path)
    p.add_argument('--backend',choices=('d3d11','opengl','metal'),default='d3d11' if os.name=='nt' else 'metal')
    p.add_argument('--samples',type=int,default=0)
    a=p.parse_args(); directory=a.angle.resolve()
    cookie=os.add_dll_directory(str(directory)) if os.name=='nt' else None
    loader=C.WinDLL if os.name=='nt' else C.CDLL
    suffix='.dll' if os.name=='nt' else '.dylib'
    egl=loader(str(directory/('libEGL'+suffix)))
    gles=loader(str(directory/('libGLESv2'+suffix)))
    address=egl.eglGetProcAddress;address.restype=C.c_void_p;address.argtypes=[C.c_char_p]
    def resolve(name):
        if name == b'huskCreateMetalFrameImage':
            try: return C.cast(bridge.tl_gl_frame_metal_image,C.c_void_p).value
            except AttributeError: return None
        ptr=address(name)
        if not ptr:
            for lib in (egl,gles):
                try: return C.cast(getattr(lib,name.decode()),C.c_void_p).value
                except AttributeError: pass
        return ptr
    def fn(name,result,*args):
        ptr=resolve(name.encode());assert ptr,name
        return C.CFUNCTYPE(result,*args)(ptr)
    P=C.c_void_p;I=C.c_int;U=C.c_uint
    arr=lambda *v:(I*len(v))(*v)
    display=fn('eglGetPlatformDisplayEXT',P,U,P,C.POINTER(I))(0x3202,None,arr(0x3203,{'d3d11':0x3208,'opengl':0x320d,'metal':0x3489}[a.backend],0x3038))
    assert display
    assert fn('eglInitialize',U,P,P,P)(display,None,None)
    assert fn('eglBindAPI',U,U)(0x30a0)
    cfg=P();n=I()
    assert fn('eglChooseConfig',U,P,P,P,I,P)(display,arr(0x3033,1,0x3040,0x44,0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3031,a.samples,0x3038),C.byref(cfg),1,C.byref(n)) and n.value
    create=fn('eglCreateContext',P,P,P,P,P)
    consumer=create(display,cfg,None,arr(0x3098,2,0x3038));producer=create(display,cfg,None,arr(0x3098,3,0x3038))
    assert consumer and producer
    pb=fn('eglCreatePbufferSurface',P,P,P,P)
    width,height=2420,1668
    surface=pb(display,cfg,arr(0x3057,width,0x3056,height,0x3038))
    ui=pb(display,cfg,arr(0x3057,32,0x3056,32,0x3038));assert surface and ui
    current=fn('eglMakeCurrent',U,P,P,P,P)
    assert current(display,ui,ui,consumer)
    string=fn('glGetString',C.c_char_p,U)
    print('Renderer:',string(0x1f01).decode());print('Consumer:',string(0x1f02).decode())
    egl_extensions=fn('eglQueryString',C.c_char_p,P,I)(display,0x3055).decode().split()
    print('Sync mode:','EGL fences/server waits' if all(x in egl_extensions for x in ('EGL_KHR_fence_sync','EGL_KHR_wait_sync')) else 'real glFinish fallback')
    print('Requested MSAA samples:',a.samples)
    bridge=C.CDLL(str(a.bridge.resolve()))
    callback=C.CFUNCTYPE(P,C.c_char_p)(resolve)
    bridge.tl_gl_frame_create.argtypes=[P,P];bridge.tl_gl_frame_create.restype=P
    bridge.tl_gl_frame_publish.argtypes=[P,P,I,I];bridge.tl_gl_frame_publish.restype=C.c_bool
    bridge.tl_gl_frame_acquire.argtypes=[P,C.POINTER(U)];bridge.tl_gl_frame_acquire.restype=C.c_bool
    for name in ('release','reset','destroy','take_guest_error'):
        getattr(bridge,'tl_gl_frame_'+name).argtypes=[P]
    bridge.tl_gl_frame_take_guest_error.restype=U
    frame=bridge.tl_gl_frame_create(callback,display)
    if not frame:
        print('EGL extensions:',fn('eglQueryString',C.c_char_p,P,I)(display,0x3055).decode())
        print('GL image extensions:',[x for x in string(0x1f03).decode().split() if 'EGL' in x])
        raise AssertionError('Real driver lacks required image/fence extensions')
    color=fn('glClearColor',None,C.c_float,C.c_float,C.c_float,C.c_float);clear=fn('glClear',None,U)
    get=fn('glGetIntegerv',None,U,P);error=fn('glGetError',U)
    gen=fn('glGenTextures',None,I,P);bind=fn('glBindTexture',None,U,U)
    genfb=fn('glGenFramebuffers',None,I,P);bindfb=fn('glBindFramebuffer',None,U,U)
    attach=fn('glFramebufferTexture2D',None,U,U,U,U,I);status=fn('glCheckFramebufferStatus',U,U)
    read=fn('glReadPixels',None,I,I,I,I,U,U,P);finish=fn('glFinish',None)
    target=U();genfb(1,C.byref(target))
    # Exercise the actual compositor operation: sample the shared image with a
    # shader into a consumer-owned framebuffer. Do not attach the imported
    # private Metal texture itself for CPU readback (not a production operation).
    output=U();gen(1,C.byref(output));bind(0xde1,output)
    fn('glTexImage2D',None,U,I,I,I,I,I,U,U,P)(0xde1,0,0x1908,32,32,0,0x1908,0x1401,None)
    bindfb(0x8d40,target);attach(0x8d40,0x8ce0,0xde1,output,0)
    assert status(0x8d40)==0x8cd5
    shader_new=fn('glCreateShader',U,U)
    shader_src=fn('glShaderSource',None,U,I,C.POINTER(C.c_char_p),P)
    compile_shader=fn('glCompileShader',None,U)
    shader_status=fn('glGetShaderiv',None,U,U,P)
    shaders=[]
    for kind,source in ((0x8b31,b'attribute vec2 p; varying vec2 uv; void main(){uv=p*0.5+0.5;gl_Position=vec4(p,0.,1.);}'),
                        (0x8b30,b'precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){gl_FragColor=texture2D(t,uv);}')):
        sh=shader_new(kind);src=C.c_char_p(source);shader_src(sh,1,C.byref(src),None);compile_shader(sh)
        value=I();shader_status(sh,0x8b81,C.byref(value));assert value.value
        shaders.append(sh)
    program=fn('glCreateProgram',U)()
    for sh in shaders: fn('glAttachShader',None,U,U)(program,sh)
    fn('glBindAttribLocation',None,U,U,C.c_char_p)(program,0,b'p')
    fn('glLinkProgram',None,U)(program)
    value=I();fn('glGetProgramiv',None,U,U,P)(program,0x8b82,C.byref(value));assert value.value
    fn('glUseProgram',None,U)(program)
    vbo=U();fn('glGenBuffers',None,I,P)(1,C.byref(vbo))
    fn('glBindBuffer',None,U,U)(0x8892,vbo)
    vertices=(C.c_float*8)(-1,-1,1,-1,-1,1,1,1)
    fn('glBufferData',None,U,C.c_ssize_t,P,U)(0x8892,C.sizeof(vertices),vertices,0x88e4)
    fn('glVertexAttribPointer',None,U,I,U,U,I,P)(0,2,0x1406,0,0,None)
    fn('glEnableVertexAttribArray',None,U)(0)
    fn('glViewport',None,I,I,I,I)(0,0,32,32)
    draw=fn('glDrawArrays',None,U,I,I)
    def consume(texture):
        bindfb(0x8d40,target);attach(0x8d40,0x8ce0,0xde1,output,0)
        bind(0xde1,texture);draw(0x0005,0,4)
        assert error()==0
    color(0,0,1,1);clear(0x4000)
    baseline=(C.c_ubyte*4)();read(0,0,1,1,0x1908,0x1401,baseline)
    assert list(baseline)==[0,0,255,255]
    print('Consumer-owned framebuffer readback baseline passed',flush=True)
    assert current(display,surface,surface,producer)
    guest=U();gen(1,C.byref(guest));bind(0xde1,guest)
    dummy=U();genfb(1,C.byref(dummy))
    tex=U();pixel=(C.c_ubyte*4)()
    times=[]
    for index in range(30):
        assert current(display,surface,surface,producer)
        bindfb(0x8d40,0);color(index%2,1-index%2,0,1);clear(0x4000)
        bindfb(0x8d40,dummy)
        # Guest GL error must survive the bridge's internal checks.
        bind(0xdead,0)
        start=time.perf_counter()
        if index == 0: print('First producer publish',flush=True)
        assert bridge.tl_gl_frame_publish(frame,surface,width,height),('publish',hex(error()))
        assert bridge.tl_gl_frame_take_guest_error(frame)==0x500
        value=I();get(0x8069,C.byref(value));assert value.value==guest.value
        get(0x8caa,C.byref(value));assert value.value==dummy.value
        assert error()==0
        assert current(display,ui,ui,consumer)
        if index == 0: print('First consumer acquire',flush=True)
        assert bridge.tl_gl_frame_acquire(frame,C.byref(tex)),('acquire',hex(error()))
        consume(tex)
        read(0,0,1,1,0x1908,0x1401,pixel)
        assert list(pixel)==[255*(index%2),255*(1-index%2),0,255],list(pixel)
        bridge.tl_gl_frame_release(frame)
        assert error()==0
        times.append((time.perf_counter()-start)*1000)
    print('PASS: 30 real GPU image transfers and compositor shader draws at 2420x1668 across nonsharing ES3/ES2 contexts; exact pixel colors, guest framebuffer/texture/error preserved')
    print(f'Host copy/fence/verification-readback mean {sum(times)/len(times):.2f} ms (includes test-only pixel read; not iPad FPS)')
    # Queue several producer/consumer turns with no CPU readback/finish between
    # them. Snapshot each consumption on the GPU, then verify all snapshots.
    # This catches reuse hazards that a per-frame synchronous read could hide.
    snapshots=(U*30)();gen(30,snapshots)
    copy_image=fn('glCopyTexImage2D',None,U,I,U,I,I,I,I,I)
    for index in range(30):
        assert current(display,surface,surface,producer)
        bindfb(0x8d40,0);color(index%2,1-index%2,0,1);clear(0x4000)
        assert bridge.tl_gl_frame_publish(frame,surface,width,height)
        assert current(display,ui,ui,consumer)
        assert bridge.tl_gl_frame_acquire(frame,C.byref(tex))
        consume(tex)
        bind(0xde1,snapshots[index]);copy_image(0xde1,0,0x1908,0,0,1,1,0)
        assert error()==0
        bridge.tl_gl_frame_release(frame)
    for index in range(30):
        attach(0x8d40,0x8ce0,0xde1,snapshots[index],0)
        assert status(0x8d40)==0x8cd5
        read(0,0,1,1,0x1908,0x1401,pixel)
        assert list(pixel)==[255*(index%2),255*(1-index%2),0,255],(index,list(pixel))
    print('PASS: 30 queued reuse cycles without per-frame CPU reads; every GPU snapshot retains its original color')
    # A uniform color cannot detect image inversion. Verify asymmetric corners
    # through the same producer copy and consumer shader, before UI transforms.
    assert current(display,surface,surface,producer)
    bindfb(0x8d40,0);color(1,0,0,1);clear(0x4000)
    fn('glEnable',None,U)(0x0c11)
    fn('glScissor',None,I,I,I,I)(0,0,width//2,height//2)
    color(0,0,1,1);clear(0x4000)
    fn('glDisable',None,U)(0x0c11)
    assert bridge.tl_gl_frame_publish(frame,surface,width,height)
    assert current(display,ui,ui,consumer)
    assert bridge.tl_gl_frame_acquire(frame,C.byref(tex));consume(tex)
    for x,y,expected in ((4,4,[0,0,255,255]),(27,4,[255,0,0,255]),
                         (4,27,[255,0,0,255]),(27,27,[255,0,0,255])):
        read(x,y,1,1,0x1908,0x1401,pixel)
        assert list(pixel)==expected,(x,y,list(pixel))
    bridge.tl_gl_frame_release(frame)
    print('PASS: asymmetric image corners retain orientation and alpha through compositor sampling')
    assert current(display,surface,surface,producer)
    bridge.tl_gl_frame_reset(frame)
    assert current(display,ui,ui,consumer)
    bridge.tl_gl_frame_destroy(frame)
    finish()
    print('Not a physical iPad or full Unity performance test.')

if __name__=='__main__':main()
