# Controller layout artwork

The full controller diagrams, including their button markings, are by **Zacksly**:

- [Xbox Series Button Icons and Controls](https://zacksly.itch.io/xbox-series-button-icons-and-controls)
- [PS5 Button Icons and Controls](https://zacksly.itch.io/ps5-button-icons-and-controls)
- [Switch Button Icons and Controls](https://zacksly.itch.io/switch-button-icons-and-controls)

Licensed under [Creative Commons Attribution 3.0 Unported](https://creativecommons.org/licenses/by/3.0/).
The three original license/attribution files are included as `LICENSE-xbox.txt`,
`LICENSE-ps.txt`, and `LICENSE-switch.txt`.

The SVGs are the original white-outline controller images from those packs.
The PNGs are modified versions: cropped to their visible bounds, resized to fit
748 × 748, and centered on transparent 768 × 768 squares. No button markings
were removed. The screen adds its own labels and leader lines at runtime.

To reproduce from the packs' supplied white-outline 4k PNGs with ImageMagick:

```sh
magick "Outline White 4k.png" -trim +repage -resize 748x748 -gravity center -background none -extent 768x768 PNG32:pad_layout.png
```

Use the PS5 image for `ps_layout.png` and the Pro Controller image for
`ns_layout.png`. Button centers in `src/deusex/gamepad_layout.cc` are normalized
against these cropped square images; update them if the crop or artwork changes.

Artwork and the attribution files ship with Ambrosia. The button glyphs beside the
labels are Kenney's Input Prompts (`assets/gamepad/prompts`); these packs are used for the central
controller illustrations only.
