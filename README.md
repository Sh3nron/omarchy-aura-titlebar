# aura-titlebar

A hover-revealed title bar plugin for Hyprland. The title bar **slides down
over the window** with a springy, iOS-like animation while the cursor hovers
the window's top strip, and disappears completely when the cursor leaves —
so only your compositor border remains by default. Close, maximize, and
minimize only exist while the bar is showing.

Based on [hyprwm/hyprland-plugins](https://github.com/hyprwm/hyprland-plugins)
`hyprbars` (BSD-3-Clause, © Vaxry & contributors, see `LICENSE`), heavily
redesigned.

## Differences from stock hyprbars

| | hyprbars | aura-titlebar |
| --- | --- | --- |
| Space | reserves a strip (window gets shorter) | **zero layout shift** — overlays the window |
| Visibility | always | **hover of the top strip only** |
| Motion | none | **spring slide-down** (reuses the `windowsIn` animation curve) |
| Border | optional own border (`bar_precedence_over_border`) | **none** — the user's border is the only border |
| Clicks when hidden | always captured ("invisible but clickable" bug #690) | **pass through to the app** |
| Fullscreen | rule-driven | also auto-suppressed |

## Install

```sh
cd ~/Projects/aura-titlebar
./setup.sh
```

`setup.sh` rebuilds the plugin against the system Hyprland headers and
installs it under a versioned filename (atomic rename) — then loads it via
`hyprctl plugin load`. A load hook in `~/.config/hypr/autostart.lua` picks
the newest version at every login.

**The one rule that produces session-killing aborts if broken:** never
truncate/overwrite a `.so` the running compositor has mapped. Builds are
installed by rename to `aura_titlebar-<timestamp>.so`; loading a rebuilt
version in-session means pointing `hyprctl plugin load` at the new file —
or simply re-login. Unload/reload of the same plugin in-session is not
part of any supported flow.

## Remove

```sh
cd ~/Projects/aura-titlebar
./uninstall.sh
```

## Config

In `~/.config/hypr/looknfeel.lua`, inside the guarded
`hl.plugin.aura_titlebar` block (`reveal_on_hover = true` is the whole point
of this fork; set it `false` to get a conventional always-on bar):

| option | default | notes |
| --- | --- | --- |
| `bar_height` | `28` | strip height and slide distance |
| `bar_color` / `col.text` | theme | pulled from the omarchy theme at reload |
| `bar_text_*`, `bar_padding`, `bar_button_padding` | — | as in hyprbars |
| `bar_buttons_alignment` | `right` | — |
| `icon_on_hover` | `false` | icons understood as hyprbars |
| `reveal_on_hover` | `true` | hover-to-appear; also what removes clicks when hidden |
| `bar_blur` | `false` | needs global blur enabled |
| `on_double_click` | — | as in hyprbars |

Buttons are registered with `hl.plugin.aura_titlebar.add_button({ ... })`
(right-to-left render order, as in hyprbars): the current setup wires
`×` → close, `□` → maximize, `−` → `omarchy-shell window-controls minimize`.

Dynamic window rules: `aura_titlebar:no_bar`, `aura_titlebar:bar_color`,
`aura_titlebar:title_color` (same syntax as hyprbars rules).

## Behavior notes

- The reveal animation **shares the `windowsIn` spring curve**, so tune it
  via `hl.animation({ leaf = "windowsIn", ... })` in `looknfeel.lua`.
- Scheduled clicks pass through to the app while the bar is hidden; the bar
  becomes interactive at ~60% of the slide.
- Pressing-and-holding the strip keeps the bar up even if the cursor leaves
  (dragging a window away mid-press works).
- During hover, a press on the strip (non-button area) drags the window and
  swallows the click, exactly like a real title bar.
- Fullscreen and `no_bar`-ruled windows never reveal the bar.

## Build layout

- `barDeco.cpp/.hpp` — the decoration: hover tracking, reveal animation,
  overlay geometry (top strip), clipping, buttons.
- `main.cpp` — plugin registration, config values, Lua `add_button`.
- `CMakeLists.txt` — standard build; the `-Wl,--no-gnu-unique` link flag
  (via `LINK_FLAGS`) is **required** — omitting it aborts Hyprland inside
  `dlsym` on load.
- `setup.sh` / `uninstall.sh` — install/remove.
