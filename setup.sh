#!/bin/bash
# Build and load aura-titlebar from ~/Projects/aura-titlebar
set -euo pipefail
cd "$(dirname "$0")"

PKG_CONFIG_PATH=/usr/share/pkgconfig cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
PKG_CONFIG_PATH=/usr/share/pkgconfig cmake --build build -j"$(nproc)"
cp -f build/libaura_titlebar.so aura_titlebar.so

echo "⏳ Loading plugin…"
if hyprctl plugin load "$(pwd)/aura_titlebar.so"; then
  echo "✅ aura-titlebar loaded"
else
  echo "❌ Load failed — see Hyprland crash report if the session died"
  exit 1
fi

echo "⏳ Reloading omarchy hook config…"
omarchy restart hyprctl >/dev/null 2>&1 || hyprctl version >/dev/null
echo "✅ done — hover the top of a window to see the bar"
