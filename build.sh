meson_options=(
    --cross-file lib32
    -D android-libbacktrace=disabled
    -D b_ndebug=true
    -D dri3=enabled
    -D egl=enabled
    -D gallium-drivers=radeonsi,virgl,svga,swrast,crocus,zink
    -D gallium-extra-hud=true
    -D gallium-nine=true
    -D gallium-omx=disabled
    -D gallium-opencl=icd
    -D gallium-rusticl=true
    -D gallium-va=enabled
    -D gallium-vdpau=enabled
    -D gallium-xa=enabled
    -D gbm=enabled
    -D gles1=disabled
    -D gles2=enabled
    -D glvnd=true
    -D glx=dri
    -D intel-clc=enabled
    -D libunwind=disabled
    -D llvm=enabled
    -D lmsensors=enabled
    -D microsoft-clc=disabled
    -D osmesa=true
    -D platforms=x11,wayland
    -D rust_std=2021
    -D shared-glapi=enabled
    -D valgrind=disabled
    -D video-codecs=vc1dec,h264dec,h264enc,h265dec,h265enc
    -D vulkan-drivers=amd,swrast,virtio
    -D vulkan-layers=device-select,overlay
    -D vulkan-beta=true
    -D opencl-spirv=true
)

# Build only minimal debug info to reduce size
#CFLAGS+=' -g1'
#CXXFLAGS+=' -g1'

export BINDGEN_EXTRA_CLANG_ARGS="-m32"

arch-meson . build "${meson_options[@]}"
meson configure build --no-pager # Print config

#if [ ! -f "build/build.ninja.bak" ]; then
#  cp build/build.ninja build/build.ninja.back
#fi

# Evil: Hack build to make proc-macro crate native
# Should become unnecessary with Meson 1.3
#sed -e '/^rule rust_COMPILER$/irule rust_HACK\n command = rustc -C linker=gcc $ARGS $in\n deps = gcc\n depfile = $targetdep\n description = Compiling native Rust source $in\n' \
#    -e '/^build src\/gallium\/frontends\/rusticl\/librusticl_proc_macros\.so:/s/rust_COMPILER/rust_HACK/' \
#    -e '/^ LINK_ARGS =/s/ src\/gallium\/frontends\/rusticl\/librusticl_proc_macros\.so//' \
#    -i build/build.ninja

$NINJAFLAGS meson compile -C build
