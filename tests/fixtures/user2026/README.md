# user2026 slide fixtures

Slides from <https://slides.mitchelloharawild.com/user2026/> (Quarto
revealjs "letterbox" deck; photos from Unsplash), used to tune the
"Background images" avoidance strategy with `tests/eval_fixtures.c`:

```sh
./build_x86_64/eval_fixtures tests/fixtures/user2026 [strategy_id] [debug_out_dir]
```

`labels.txt` is the machine-readable list the tool reads.

| File | Slide (`#/N`) | Content | Expected image side |
|---|---|---|---|
| slide00.ppm | 0 | title, flat-colour pattern block, links box | none |
| slide01.ppm | 1 | two hex logos + arrow (SVG), centred | none |
| slide04.ppm | 4 | title; pencils photo (dark) right | right |
| slide07.ppm | 7 | callout; playing-cards photo right | right |
| slide11.ppm | 11 | code + output; cards photo right | right |
| slide15.ppm | 15 | hex logo, callout, code; paint-roller photo right | right |
| slide22.ppm | 22 | tall code + output; paint-roller photo right | right |
| slide23.ppm | 23 | 3×3 grid diagram (SVG) | none |
| slide26.ppm | 26 | grid diagram with flat colour fills (SVG) | none |
| slide30.ppm | 30 | dice photo (pale) left; callout + code | left |
| slide32.ppm | 32 | dice photo left; list callout | left |
| slide36.ppm | 36 | crystal-ball photo left; title only | left |
| slide39.ppm | 39 | palette photo left; bar-chart screenshots | left |
| slide43.ppm | 43 | "Bonus slides" centred text | none |
| slide46.ppm | 46 | clover photo right; callout | right |
| slide53.ppm | 53 | palette photo left; code + density plot | left |

Fragments are in their initial (hidden) state.

## How they were made

Rendered at 1920×1080 with headless Chrome:

```sh
google-chrome --headless=new --disable-gpu --hide-scrollbars \
  --window-size=1920,1080 --virtual-time-budget=8000 \
  --screenshot=sN.png 'https://slides.mitchelloharawild.com/user2026/#/N'
```

then downscaled to 320×180 the way the plugin's capture does it: OBS draws
the source into a small render target with plain bilinear sampling (no
mipmaps), so each output pixel is a bilinear tap at
`(x + 0.5) * 6 - 0.5` in the source — aliased and sharper than a box filter.
Stored as binary PPM (P6) so a dependency-free C program can read them.
