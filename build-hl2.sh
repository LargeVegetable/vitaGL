#!/bin/sh
# Builds vitaGL the way the HL2 Vita port uses it:
#   LOG_ERRORS=1          error messages via sceClibPrintf (the launcher sends them to its log)
#   HAVE_SHARK_LOG=1      shader compiler output in shader info logs
#   SHARED_RENDERTARGETS=2  framebuffers share sceGxm render targets, recycling the
#                         oldest when out of handles; Source creates more render
#                         textures than sceGxm has render targets
#   SAFE_UNIFORMS=1       uniform locations for array elements ("vc[12]"), which ToGL
#                         uploads all shader constants through
#   SINGLE_THREADED_GC=1  garbage collection inside vglSwapBuffers: the GC thread freed
#                         into the sceClibMspace heaps while the engine allocated from
#                         them, with nothing serializing the two
#   HAVE_SHADER_CACHE=1   compiled shaders cached in ux0:data/shader_cache/<title id>,
#                         keyed by a hash of their source, so only the first run
#                         compiles them on the device
#   STORE_DEPTH_STENCIL=1 depth/stencil stored at the end of every sceGxm scene and
#                         loaded at the start of the next: without it, a render target
#                         switch mid-frame (framebuffer copies for glass, Breencast
#                         monitors) lost the depth buffer, and everything drawn after it
#                         showed through walls
#   HEAP_CANARIES=1       (set in the environment) heap overflow detector, slow
set -e
: "${VITASDK:?set VITASDK to the VitaSDK install}"
export PATH="$VITASDK/bin:$PATH"
cd "$(dirname "$0")"
make clean > /dev/null
make LOG_ERRORS=1 HAVE_SHARK_LOG=1 SHARED_RENDERTARGETS=2 SAFE_UNIFORMS=1 SINGLE_THREADED_GC=1 HAVE_SHADER_CACHE=1 STORE_DEPTH_STENCIL=1 HEAP_CANARIES=${HEAP_CANARIES:-0} -j8
