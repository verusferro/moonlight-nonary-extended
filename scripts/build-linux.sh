#!/usr/bin/env bash
# Linux build: patched SDL3 -> sdl2-compat (pinned) -> Moonlight client -> staged dir.
# Idempotent; re-run to rebuild. Needs cmake, ninja, qmake6 and the usual Moonlight -dev packages.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"      # vibepollo/
APP="$ROOT/moonlight-nonary"
SDL3_SRC="${SDL3_SRC:-$ROOT/moonlight-sdl}"
COMPAT_SRC="${COMPAT_SRC:-$ROOT/sdl2-compat}"
COMPAT_COMMIT=a53b6ad90ecd2d0ccfe01d5cfd2059793acf8c12
STAGE="${STAGE:-$APP/build-linux/stage}"
JOBS="${JOBS:-$(nproc)}"

# 1. Patched SDL3 (shared, HIDAPI). XTEST off: libxtst-dev not installed; harmless for streaming.
cmake -S "$SDL3_SRC" -B "$SDL3_SRC/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_HIDAPI=ON -DSDL_HIDAPI_JOYSTICK=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DSDL_X11_XTEST=OFF
cmake --build "$SDL3_SRC/build-linux" -j"$JOBS"

# 2. sdl2-compat at pinned commit, linked against the build above
[ -d "$COMPAT_SRC/.git" ] || git clone https://github.com/libsdl-org/sdl2-compat "$COMPAT_SRC"
git -C "$COMPAT_SRC" cat-file -e "$COMPAT_COMMIT^{commit}" 2>/dev/null || git -C "$COMPAT_SRC" fetch origin
git -C "$COMPAT_SRC" checkout -q "$COMPAT_COMMIT"
cmake -S "$COMPAT_SRC" -B "$COMPAT_SRC/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSDL3_DIR="$SDL3_SRC/build-linux" -DSDL2COMPAT_TESTS=OFF -DSDL2COMPAT_INSTALL=OFF \
  -DCMAKE_BUILD_RPATH="$SDL3_SRC/build-linux"
cmake --build "$COMPAT_SRC/build-linux" -j"$JOBS"

# 3. Client (compiles against system SDL2 headers; SDL3 symbols are looked up at runtime)
mkdir -p "$APP/build-linux/app"
(cd "$APP/build-linux" && qmake6 CONFIG+=release "$APP/moonlight-qt.pro" && make -j"$JOBS")

# 4. Stage: binary + our SDL3 + sdl2-compat, launcher sets LD_LIBRARY_PATH
rm -rf "$STAGE"; mkdir -p "$STAGE/lib"
cp -a "$APP/build-linux/app/moonlight" "$STAGE/moonlight.bin"
cp -a "$SDL3_SRC"/build-linux/libSDL3.so* "$COMPAT_SRC"/build-linux/libSDL2-2.0.so* "$STAGE/lib/"
cat > "$STAGE/moonlight" <<'EOF'
#!/bin/sh
d="$(cd "$(dirname "$0")" && pwd)"
LD_LIBRARY_PATH="$d/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$d/moonlight.bin" "$@"
EOF
chmod +x "$STAGE/moonlight"
echo "Staged: $STAGE/moonlight"
