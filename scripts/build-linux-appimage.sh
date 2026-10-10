#!/usr/bin/env bash
# AppImage that matches upstream Moonlight's: FFmpeg 9 / libplacebo / libva / dav1d built into a local prefix
# (build-linux/appimage-deps, no sudo), Moonlight linked against it, host libva preferred with a bundled fallback.
# Run scripts/build-linux.sh first (provides the patched SDL3 + sdl2-compat in build-linux/stage).
# Needs the usual Moonlight -dev packages plus meson, ninja, cmake, nasm, libx11-xcb-dev, libxcb-dri3-dev, libshaderc-dev,
# Vulkan headers >= 1.4.313 (built below), and linuxdeploy + linuxdeploy-plugin-qt (x86_64 AppImages) in PATH.
# Runs on distributions with glibc >= the build machine's (Ubuntu 24.04: 2.39). GPU/Wayland/EGL/Vulkan drivers come from the host.
set -euo pipefail

APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$APP/build-linux"
STAGE="${STAGE:-$OUT/stage}"
SRC="$OUT/appimage-src"
P="$OUT/appimage-deps"
AD="$OUT/AppDir"
JOBS="${JOBS:-$(nproc)}"
ID=moonlight-nonary-extended
export PKG_CONFIG_PATH="$P/lib/pkgconfig" LD_LIBRARY_PATH="$P/lib:$STAGE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# clone <url> <tag> <dir>: shallow checkout of a tag (or commit) once
clone() {
  [ -d "$SRC/$3/.git" ] && return 0
  mkdir -p "$SRC"; rm -rf "$SRC/$3"
  git clone -q --recursive --branch "$2" --depth 1 "$1" "$SRC/$3" 2>/dev/null || {
    git clone -q --recursive "$1" "$SRC/$3"; git -C "$SRC/$3" checkout -q "$2"; git -C "$SRC/$3" submodule update -q --init --recursive; }
}

# 1. Dependency prefix (versions as in upstream's .github/workflows/build-appimage.yml)
if [ ! -e "$P/lib/libavcodec.so" ]; then
  mkdir -p "$P/lib" "$P/include"
  # Vulkan headers: FFmpeg 9 needs >= 1.3.277, Ubuntu 24.04 has 1.3.275 (upstream gets 1.4.313 from the LunarG SDK)
  clone https://github.com/KhronosGroup/Vulkan-Headers v1.4.313 Vulkan-Headers
  cmake -S "$SRC/Vulkan-Headers" -B "$SRC/Vulkan-Headers/build" -DCMAKE_INSTALL_PREFIX="$P" >/dev/null
  cmake --install "$SRC/Vulkan-Headers/build" >/dev/null

  clone https://github.com/intel/libva 2.24.1 libva
  (cd "$SRC/libva" && rm -rf build && CFLAGS=-I/usr/include/xcb meson setup build --prefix="$P" --libdir=lib \
     -Dwith_x11=yes -Dwith_glx=no -Dwith_wayland=no -Ddisable_drm=true >/dev/null && ninja -C build install >/dev/null)

  clone https://github.com/haasn/libplacebo 92b5ac6db79f4d680eb656692f7bf51e9606f42a libplacebo
  (cd "$SRC/libplacebo" && git apply "$APP"/app/deploy/linux/appimage/*.patch &&
   meson setup build --prefix="$P" --libdir=lib -Dvulkan=enabled -Dopengl=disabled -Ddemos=false >/dev/null &&
   ninja -C build install >/dev/null)

  clone https://code.videolan.org/videolan/dav1d.git 1.5.4 dav1d
  (cd "$SRC/dav1d" && meson setup build --prefix="$P" --libdir=lib -Ddefault_library=static -Dbuildtype=release \
     -Denable_tools=false -Denable_tests=false >/dev/null && ninja -C build install >/dev/null)

  # libva above has no DRM backend, so FFmpeg must not link the host's libva-drm (the AppImage would not carry a matching
  # one): hide the host's .pc file.
  printf 'Name: libva-drm\nDescription: hidden\nVersion: 0\nRequires: libva-drm-hidden\n' > "$P/lib/pkgconfig/libva-drm.pc"
  clone https://github.com/FFmpeg/FFmpeg n9.0.2 FFmpeg
  (cd "$SRC/FFmpeg" && ./configure --prefix="$P" --libdir="$P/lib" --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib" \
     --enable-pic --enable-lto --disable-static --enable-shared --disable-all --disable-autodetect \
     --enable-avcodec --enable-avformat --enable-swscale \
     --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=av1 \
     --enable-vaapi --enable-hwaccel=h264_vaapi --enable-hwaccel=hevc_vaapi --enable-hwaccel=av1_vaapi \
     --enable-vdpau --enable-hwaccel=h264_vdpau --enable-hwaccel=hevc_vdpau --enable-hwaccel=av1_vdpau \
     --enable-libdrm --enable-vulkan --enable-hwaccel=h264_vulkan --enable-hwaccel=hevc_vulkan --enable-hwaccel=av1_vulkan \
     --enable-libdav1d --enable-decoder=libdav1d &&
   make -j"$JOBS" && make install)
fi

# 2. Moonlight against that prefix (as upstream's scripts/build-appimage.sh: no Wayland/DRM in the client, APP_IMAGE define)
B="$OUT/appimage-build"
rm -rf "$B"; mkdir -p "$B"
(cd "$B" && qmake6 CONFIG+=release CONFIG+=disable-wayland CONFIG+=disable-libdrm DEFINES+=APP_IMAGE "$APP/moonlight-qt.pro" && make -j"$JOBS")

# 3. AppDir
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE=qmake6 QML_SOURCES_PATHS="$APP/app/gui"
# Wayland (native on KDE/GNOME) and offscreen (for --help smoke tests) in addition to xcb
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="wayland-decoration-client;wayland-shell-integration"
export VERSION="$(cat "$APP/app/version.txt")"

rm -rf "$AD"; mkdir -p "$AD/usr/bin" "$AD/usr/lib"
cp "$B/app/moonlight" "$AD/usr/bin/moonlight"
cp -a "$STAGE"/lib/* "$AD/usr/lib/"
cp "$APP/app/res/moonlight.svg" "$AD/$ID.svg"
cat > "$AD/$ID.desktop" <<EOT
[Desktop Entry]
Name=Moonlight Nonary Extended
Exec=moonlight
Icon=$ID
Terminal=false
Type=Application
Categories=Qt;Game;Network;RemoteAccess;
EOT

# libva: stage the build-environment copy in opt/libva-fallback (outside usr/, so linuxdeploy ignores it) with a probe binary
# carrying Moonlight's libva ELF requirements; AppRun makes the copy visible only if the host libva cannot satisfy them.
FB="$AD/opt/libva-fallback"
mkdir -p "$FB"
cp -a "$P"/lib/libva*.so* "$FB/"
cc -O2 -I"$P/include" -L"$P/lib" -Wl,--no-as-needed -Wl,-rpath-link,"$P/lib" -o "$FB/libva-probe" \
  "$APP/app/deploy/linux/libva-probe.c" -lva -lva-x11
# The probe must reference every VA_API_* node required by what ships in the AppImage
va_nodes() { LC_ALL=C readelf -V "$1" 2>/dev/null | { grep -oE 'VA_API_[0-9]+\.[0-9]+\.[0-9]+' || true; } | sort -u; }
NEEDED_NODES=$(for b in "$AD/usr/bin/moonlight" "$P"/lib/libav*.so* "$P"/lib/libsw*.so*; do [ -f "$b" ] && va_nodes "$b"; done | sort -u)
PROBE_NODES=$(va_nodes "$FB/libva-probe")
MISSING=$(comm -13 <(echo "$PROBE_NODES") <(echo "$NEEDED_NODES"))
[ -z "$MISSING" ] || { echo "libva-probe is missing version node(s): $MISSING - update app/deploy/linux/libva-probe.c!" >&2; exit 1; }

# AppRun: from upstream's AppRun-libva (prefer host libva; fallback copy only if the probe cannot start), plus our SDL3/sdl2-compat in usr/lib
cat > "$OUT/AppRun-libva" <<'EOT'
#!/bin/bash
# VA-API drivers on the host export __vaDriverInit_1_XX for the libva they were built against and a libva cannot load newer
# drivers than itself, so a bundled (older) libva breaks hardware decoding on up-to-date distros. libva-probe is linked against
# the same versioned libva symbols as Moonlight/FFmpeg: if the loader can start it, the host libva is used; otherwise
# (no libva, or too old) the staged build-environment copy is made visible.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
LIBVA_FALLBACK="$APPDIR/opt/libva-fallback"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -d "$LIBVA_FALLBACK" ] && ! "$LIBVA_FALLBACK/libva-probe" 2>/dev/null; then
    export LD_LIBRARY_PATH="$LIBVA_FALLBACK:$LD_LIBRARY_PATH"
    export VAAPI_USE_FALLBACK_PATHS=1
fi

exec "$APPDIR/usr/bin/moonlight" "$@"
EOT
chmod +x "$OUT/AppRun-libva"

# the Qt plugin deployer skips this one, but without it Wayland windows have no EGL/GPU buffers
QP="$(qmake6 -query QT_INSTALL_PLUGINS)/wayland-graphics-integration-client"
mkdir -p "$AD/usr/plugins/wayland-graphics-integration-client"
cp "$QP/libqt-plugin-wayland-egl.so" "$AD/usr/plugins/wayland-graphics-integration-client/"

# 4. AppImage. libva is never bundled (see AppRun); Vulkan loader and GPU libs come from the host.
cd "$OUT"
linuxdeploy --appdir "$AD" --plugin qt --custom-apprun "$OUT/AppRun-libva" \
  --library "$AD/usr/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" \
  --exclude-library='libva.so*' --exclude-library='libva-drm.so*' --exclude-library='libva-wayland.so*' --exclude-library='libva-x11.so*' \
  --exclude-library='libvulkan.so*' \
  --output appimage --desktop-file "$AD/$ID.desktop" --icon-file "$AD/$ID.svg"
ls -la "$OUT"/*.AppImage
