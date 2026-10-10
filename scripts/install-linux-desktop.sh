#!/usr/bin/env bash
# Optional: add a start-menu entry that runs the stage launcher (patched SDL3). Run after scripts/build-linux.sh.
set -euo pipefail

APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAUNCHER="${LAUNCHER:-$APP/build-linux/stage/moonlight}"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
ID=moonlight-nonary-extended

[ -x "$LAUNCHER" ] || { echo "Launcher not found: $LAUNCHER (run scripts/build-linux.sh first)" >&2; exit 1; }
mkdir -p "$DATA/applications" "$DATA/icons/hicolor/scalable/apps"
cp "$APP/app/res/moonlight.svg" "$DATA/icons/hicolor/scalable/apps/$ID.svg"
cat > "$DATA/applications/$ID.desktop" <<EOT
[Desktop Entry]
Name=Moonlight Nonary Extended
Comment=Stream games and other applications from another PC running Sunshine or Apollo
Exec="$LAUNCHER"
Icon=$ID
Terminal=false
Type=Application
Categories=Qt;Game;Network;RemoteAccess;
Keywords=nvidia;gamestream;stream;sunshine;apollo;remote play;
EOT
command -v update-desktop-database >/dev/null && update-desktop-database "$DATA/applications" || true
echo "Installed $DATA/applications/$ID.desktop"
