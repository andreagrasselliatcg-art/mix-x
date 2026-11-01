#!/bin/bash
# Installs Mix-X (AU + VST3) for Logic Pro. Double-click this file in Finder.
set -e
cd "$(dirname "$0")"

AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$AU_DIR" "$VST3_DIR"

for B in "Mix-X.component:$AU_DIR" "Mix-X.vst3:$VST3_DIR"; do
  NAME="${B%%:*}"; DEST="${B#*:}"
  if [ -d "$NAME" ]; then
    rm -rf "$DEST/$NAME"
    cp -R "$NAME" "$DEST/"
    xattr -dr com.apple.quarantine "$DEST/$NAME" 2>/dev/null || true
    codesign --force --deep -s - "$DEST/$NAME" >/dev/null 2>&1 || true
    echo "Installed $NAME in $DEST"
  fi
done

killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aufx MixX Tntk | tail -3 || true
echo ""
echo "Done. Logic: Audio FX > Audio Units > Tune Talks > Mix-X   |   Ableton: Plug-ins > Tune Talks > Mix-X (VST3/AU)"
