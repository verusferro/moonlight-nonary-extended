#!/usr/bin/env bash
# Linux build: patched SDL3 -> sdl2-compat (pinned) -> Moonlight client -> staged dir.
# Idempotent; re-run to rebuild. Needs git, cmake, ninja, qmake6 and the usual Moonlight -dev packages.
set -euo pipefail

APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$APP/build-linux"
SDL3_TAG=release-3.4.18
SDL3_SRC="$OUT/SDL-$SDL3_TAG"
COMPAT_SRC="$OUT/sdl2-compat"
COMPAT_TAG=release-2.32.74
STAGE="${STAGE:-$OUT/stage}"
JOBS="${JOBS:-$(nproc)}"

# 0. Arch / CachyOS: stop early if build dependencies are missing (the only copy of the list)
if command -v pacman >/dev/null; then
  missing="$(pacman -T gcc make pkgconf git cmake ninja \
    qt6-base qt6-declarative qt6-svg qt6-wayland \
    ffmpeg libva libvdpau libplacebo vulkan-headers vulkan-icd-loader \
    opus sdl2-compat sdl2_ttf openssl libxkbcommon wayland wayland-protocols libdrm mesa \
    libpulse pipewire libusb systemd-libs dbus \
    libx11 libxext libxtst libxrandr libxcursor libxi libxfixes libxss | tr '\n' ' ')" || true
  if [ -n "$missing" ]; then
    echo "Missing build dependencies. Install them, then run this script again:" >&2
    echo "  sudo pacman -S --needed $missing" >&2
    exit 1
  fi
fi

# 1. SDL3 with the DualSense patches (shared, HIDAPI)
if [ ! -d "$SDL3_SRC" ]; then
  rm -rf "$SDL3_SRC.tmp"
  git clone -q --depth 1 --branch "$SDL3_TAG" https://github.com/libsdl-org/SDL "$SDL3_SRC.tmp"
  git -C "$SDL3_SRC.tmp" apply "$APP"/sdl-patches/*.patch
  mv "$SDL3_SRC.tmp" "$SDL3_SRC"
fi
cmake -S "$SDL3_SRC" -B "$SDL3_SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_HIDAPI=ON -DSDL_HIDAPI_JOYSTICK=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build "$SDL3_SRC/build" -j"$JOBS"

# 2. sdl2-compat at pinned commit, linked against the build above
[ -d "$COMPAT_SRC/.git" ] || git clone -q https://github.com/libsdl-org/sdl2-compat "$COMPAT_SRC"
git -C "$COMPAT_SRC" checkout -q "$COMPAT_TAG" 2>/dev/null || { git -C "$COMPAT_SRC" fetch -q --tags origin; git -C "$COMPAT_SRC" checkout -q "$COMPAT_TAG"; }
cmake -S "$COMPAT_SRC" -B "$COMPAT_SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSDL3_DIR="$SDL3_SRC/build" -DSDL2COMPAT_TESTS=OFF -DSDL2COMPAT_INSTALL=OFF \
  -DCMAKE_BUILD_RPATH="$SDL3_SRC/build"
cmake --build "$COMPAT_SRC/build" -j"$JOBS"

# 3. Client (compiles against system SDL2 headers; SDL3 symbols are looked up at runtime)
mkdir -p "$OUT/app"
(cd "$OUT" && qmake6 CONFIG+=release "$APP/moonlight-qt.pro" && make -j"$JOBS")

# 4. Stage: binary + our SDL3 + sdl2-compat, launcher sets LD_LIBRARY_PATH
rm -rf "$STAGE"; mkdir -p "$STAGE/lib"
cp -a "$OUT/app/moonlight" "$STAGE/moonlight.bin"
cp -a "$SDL3_SRC"/build/libSDL3.so* "$COMPAT_SRC"/build/libSDL2-2.0.so* "$STAGE/lib/"
cat > "$STAGE/moonlight" <<'EOF'
#!/bin/sh
d="$(cd "$(dirname "$0")" && pwd)"
LD_LIBRARY_PATH="$d/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$d/moonlight.bin" "$@"
EOF
chmod +x "$STAGE/moonlight"
echo "Staged: $STAGE/moonlight"
