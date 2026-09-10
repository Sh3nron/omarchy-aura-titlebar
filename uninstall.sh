#!/bin/bash
# Remove aura-titlebar entirely
set -euo pipefail

sed -i '/aura-titlebar (>/d; /aura-titlebar\/aura_titlebar-/d; /hyprctl plugin load \$(/d' ~/.config/hypr/autostart.lua 2>/dev/null || true
sed -i '/aura-titlebar plugin/d' ~/.config/hypr/autostart.lua 2>/dev/null || true
# remove the 3-line load block written by the earlier install
python3 - <<'EOF' 2>/dev/null || true
import re, os
p = os.path.expanduser("~/.config/hypr/autostart.lua")
src = open(p).read()
src = src.replace("""-- aura-titlebar (~/Projects/aura-titlebar): hover-revealed title bars that
-- slide over the app window. Loads via IPC each session; rebuild it first
-- with the project's setup.sh if it has changed.
o.exec_on_start("hyprctl plugin load /home/yeshuah/Projects/aura-titlebar/aura_titlebar.so")
""", "")
open(p, "w").write(src)
EOF

for h in $(hyprctl plugin list | grep -i "aura_titlebar" | awk '{print tolower($2)}'); do
  hyprctl plugin uninstall "$h" 2>/dev/null || hyprctl plugin destroy "$h" 2>/dev/null || true
done
hyprctl plugin list 2>&1 | head -2
echo "aura-titlebar removed. Log out & back in for a stock titlebar-free desktop."
