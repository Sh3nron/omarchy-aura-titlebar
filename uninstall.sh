#!/bin/bash
# Remove aura-titlebar and restore stock behavior
set -euo pipefail

sed -i '/aura-titlebar plugin/d; /aura-titlebar\/aura_titlebar.so/d' ~/.config/hypr/autostart.lua 2>/dev/null || true

hyprctl plugin uninstall aura_titlebar 2>&1 || true
hyprctl plugin list 2>&1 | head -2
echo "aura-titlebar removed. Log out & back in (or run 'hyprpm reload --notify') to bring back stock hyprbars."
