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

`setup.sh` rebuilds against the system Hyprland headers and installs a unique
versioned filename by atomic rename. If Aura is already loaded, the update
activates on the next login. Use `./setup.sh --stage-only` to install without
making any compositor or configuration changes. The existing load hook in
`~/.config/hypr/autostart.lua` picks the newest version at login.

**The one rule that produces session-killing aborts if broken:** never
truncate/overwrite a `.so` the running compositor has mapped. Builds are
installed by rename to `aura_titlebar-<timestamp>-<unique-id>.so`. Updates
require a new login; do not load a second copy or unload/reload in-session.
Previous binaries are retained for rollback. To roll back, move the unwanted
version outside the `aura_titlebar-*.so` filename pattern before the next login.

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

### Progressive blur

`bar_gradual_blur = true` uses five overlapping vertical masks and increasing
Gaussian blur radii, following [React Bits Gradual Blur](https://reactbits.dev/animations/gradual-blur).
It blurs the live app backdrop before drawing crisp title text and controls.
The blur stays strong across the title strip and becomes progressively weaker
below it, clipped to the window's rounded boundary. This replaces the old
single-radius blur with fading opacity and its outward halo.

- `bar_blur_reach = 96`: fade distance below the title strip, in logical pixels.
- `bar_blur_strength = 2.0`: radius multiplier, from 0 to 4; independent of global blur size/passes.
- `bar_tint_opacity = 0.20`: light theme tint, from 0 to 1, multiplied by `bar_color` alpha. Zero means blur only.
- The existing `windowsIn` spring moves the band and controls together.
- Global `decoration.blur.enabled` is respected. With gradual blur disabled,
  the conventional bar background and `bar_blur` setting are used.

GPU intermediates are reused, and only the strip plus kernel support is
filtered. Existing damage near the strip is expanded before rendering to
avoid stale backdrop pixels; an idle or hidden bar does not schedule frames.
Shader/allocation failure falls back to the conventional bar background.

### Buttons & trigger zone

- `bar_button_scale = 1.5` (default) — multiplier on every button's
  configured `size` (looknfeel values 11/10/11 become ~16px circles). Scales
  with monitor scale like everything else.
- The **reveal trigger** is the top half of the strip (14px at default
  height) — deliberate dip expected. The full strip keeps the bar alive
  once revealed, so dipping below the trigger line doesn't hide it.

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
- `CMakeLists.txt` — explicit plugin sources, including the blur pass;
  GCC compiles with `-fno-gnu-unique`. Applying that option only at link
  time does not remove GNU-unique bindings from existing object files.
- `setup.sh` / `uninstall.sh` — install/remove.

### Verification

Run the production shaders in a standalone EGL context (no plugin load):

```sh
c++ -std=c++23 tests/shaders.cpp -lEGL -lGLESv2 -o /tmp/aura-shader-test
/tmp/aura-shader-test
```

This checks shader compilation, progressive softness over fine stripes,
unchanged pixels below the fade, rounded corners, zero reach, hidden opacity,
and large/high-DPI kernels. Test plugin changes in a separate compositor
before installing. Never rebuild a library that a test compositor has mapped;
exit that test compositor first or use a new build directory.
