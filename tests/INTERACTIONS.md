# Isolated interaction regression test

Use a separate Hyprland instance with a single 1280×720, scale-1 HEADLESS-1
output and exactly one synthetic browser window titled `Aura progressive blur
check`. Use a 28px Aura titlebar, padding 8, button gap 5, and hover enabled.
Never point this test at the user's desktop. The test disables only the nested
instance's inherited `wl_pointer` so external mouse motion cannot interfere.

Build `virtual-pointer.c` with Wayland client headers generated from the official
`wlr-virtual-pointer-unstable-v1.xml` protocol:

```sh
wayland-scanner client-header "$PROTOCOL_XML" /tmp/pointer.h
wayland-scanner private-code "$PROTOCOL_XML" /tmp/pointer-protocol.c
cc -I/tmp tests/virtual-pointer.c /tmp/pointer-protocol.c -lwayland-client -o /tmp/aura-pointer
python tests/interactions.py --instance TEST_INSTANCE --wayland TEST_SOCKET --pointer /tmp/aura-pointer --output /tmp
```

The script asserts that pressing alone and 2px jitter do not detach; moving
past the threshold preserves size and the exact grab offset; subsequent native
movement preserves the anchor; release keeps the window floating and ends the
grab; button actions occur on release and cancel outside; and fullscreen
detachment also preserves geometry. It captures hover and pressed states for
visual comparison. The test button runs a maximize action explicitly scoped
to the test compositor.

Verified on Hyprland 0.56.2: 1228×668 tiled → 1228×668 floating, moving from
(26,26) to (56,66) for a (30,40) pointer delta. Fullscreen 1280×720 → 1280×720
floating at (40,40) for a (40,40) pointer delta. Hover and press captures show
the enlarged and compressed control respectively.
