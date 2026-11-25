# Icons

Each program has a folder: `ambrosia` for the game and `dented` for the editor. In each, the
source image (`ambrosia.svg`, `dented.png`) is the original, and the other files are made from it
and checked in, so the build needs no image tools:

| File | Used for |
|---|---|
| `32.png`, `64.png`, `128.png`, `256.png` | The window's icon, embedded in that program (`core/desktop/window_icon.cc`): 32 at 100 percent scale, the others as SDL's alternates for higher scales |
| `ambrosia.ico`, `dented.ico` | The Windows program's icon resource (`src/ambrosia.rc`, `src/dented.rc`), PNG frames from 16 to 256 |
| `ambrosia/switch.jpg` | The Switch `.nro`'s icon in the homebrew menu, 256x256 on a dark background (the editor has no Switch build) |

After changing a source image, remake them with ImageMagick (with its librsvg delegate, which
draws the SVG's gradients and clip path):

```powershell
nu scripts/icon.nu
```
