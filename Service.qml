import QtQuick
import Quickshell

// aura-titlebar is a Hyprland compositor plugin (C++ .so), not Quickshell
// UI. Omarchy plugins need a service entry point, so this scope stays
// resident and reports whether the compositor side is installed.
Scope {
}
