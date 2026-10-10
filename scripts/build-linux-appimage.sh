#!/usr/bin/env bash
# AppImage from the staged build (run scripts/build-linux.sh first). Bundles Qt, FFmpeg, patched SDL3 and sdl2-compat.
# Runs on distributions with glibc >= the build machine's (Ubuntu 24.04: 2.39). GPU/Wayland/EGL/Vulkan drivers come from the host.
# Needs linuxdeploy and linuxdeploy-plugin-qt (x86_64 AppImages) in PATH, and qmake6.
set -euo pipefail

APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="${STAGE:-$APP/build-linux/stage}"
AD="$APP/build-linux/AppDir"
ID=moonlight-nonary-extended
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE=qmake6 QML_SOURCES_PATHS="$APP/app/gui"
# Wayland (native on KDE/GNOME) and offscreen (for --help smoke tests) in addition to xcb
export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="wayland-decoration-client;wayland-shell-integration"
export VERSION="$(cat "$APP/app/version.txt")"
export LD_LIBRARY_PATH="$STAGE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

rm -rf "$AD"; mkdir -p "$AD/usr/bin" "$AD/usr/lib"
cp "$STAGE/moonlight.bin" "$AD/usr/bin/moonlight"
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
cat > "$AD/AppRun" <<'EOT'
#!/bin/sh
d="$(dirname "$(readlink -f "$0")")"
LD_LIBRARY_PATH="$d/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$d/usr/bin/moonlight" "$@"
EOT
chmod +x "$AD/AppRun"

# the Qt plugin deployer skips this one, but without it Wayland windows have no EGL/GPU buffers
QP="$(qmake6 -query QT_INSTALL_PLUGINS)/wayland-graphics-integration-client"
mkdir -p "$AD/usr/plugins/wayland-graphics-integration-client"
cp "$QP/libqt-plugin-wayland-egl.so" "$AD/usr/plugins/wayland-graphics-integration-client/"

cd "$APP/build-linux"
linuxdeploy --appdir "$AD" --plugin qt --library "$AD/usr/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so" --output appimage --desktop-file "$AD/$ID.desktop" --icon-file "$AD/$ID.svg"
ls -la "$APP"/build-linux/*.AppImage
