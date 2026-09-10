#!/bin/bash
# Build and (safely) load aura-titlebar from ~/Projects/aura-titlebar
#
# Cardinal rule: never truncate or overwrite a .so the running compositor
# has mapped. We install with atomic rename under a fresh, versioned
# filename so every load is against a never-before-mapped inode.
set -euo pipefail
cd "$(dirname "$0")"

PKG_CONFIG_PATH=/usr/share/pkgconfig cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
PKG_CONFIG_PATH=/usr/share/pkgconfig cmake --build build -j"$(nproc)"

STAMP="$(date +%Y%m%d%H%M%S)"
TARGET="aura_titlebar-${STAMP}.so"

# atomic install: rename(2) leaves any existing mapping intact
mv -f build/libaura_titlebar.so "${TARGET}"

echo "⏳ Loading plugin (${TARGET})…"
if hyprctl plugin list | grep -q "aura_titlebar"; then
  echo "⚠️  an aura_titlebar is already loaded. The new version file is on"
  echo "    disk and will be picked up on your next login (re-run setup.sh"
  echo "    after logging back in to load the newest file into the session)."
elif hyprctl plugin load "$(pwd)/${TARGET}"; then
  echo "✅ aura-titlebar loaded"
else
  echo "❌ Load failed — see Hyprland crash report if the session died"
  exit 1
fi

# keep only the newest 2 versions around
ls -1t aura_titlebar-*.so 2>/dev/null | tail -n +3 | xargs -r rm -f

echo "⏳ Reloading hook config…"
omarchy restart hyprctl >/dev/null 2>&1 || hyprctl version >/dev/null
echo "✅ done — hover the top of a window to see the bar"
