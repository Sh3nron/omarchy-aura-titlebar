#!/bin/bash
# Versioned installation: never replace any binary mapped by a compositor.
set -euo pipefail
cd "$(dirname "$0")"

stage_only=false
case "${1:-}" in
  --stage-only) stage_only=true ;;
  "") ;;
  *) echo "Usage: $0 [--stage-only]" >&2; exit 2 ;;
esac

PKG_CONFIG_PATH=/usr/share/pkgconfig cmake -B build -DCMAKE_BUILD_TYPE=Release
PKG_CONFIG_PATH=/usr/share/pkgconfig cmake --build build -j"$(nproc)"

# The temporary name is unique even for two builds within the same second.
staging=$(mktemp "$PWD/.aura-titlebar-install.XXXXXX")
trap 'rm -f -- "$staging"' EXIT
install -m755 build/libaura_titlebar.so "$staging"
target="$PWD/aura_titlebar-$(date +%Y%m%d%H%M%S)-${staging##*.}.so"
mv -n -- "$staging" "$target"
echo "Installed: $target"

# Keep previous versions, including binaries still mapped in other sessions.
# Staging never loads/unloads a plugin or reloads the user's configuration.
if "$stage_only"; then
  echo "Staged for next login. The running desktop is unchanged."
  exit 0
fi
if ! plugins=$(hyprctl plugin list); then
  echo "Could not inspect the running compositor; staged for next login."
  exit 0
fi
if [[ "$plugins" == *aura_titlebar* ]]; then
  echo "An aura-titlebar is already loaded; the new version activates at next login."
  exit 0
fi
hyprctl plugin load "$target"
hyprctl reload
hyprctl configerrors
