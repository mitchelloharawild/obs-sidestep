# obs-sidestep

An OBS Studio plugin that moves your camera out of the way of on-screen
content. It watches a content source (slides, screen or window capture),
finds where the content is, and moves the camera to a clear spot — your
preferred corners first, or anywhere that's least busy — hiding it when
nowhere is clear.

**Status:** early development (v0.2 feature set). See
[`_dev/design-spec.md`](_dev/design-spec.md).

## Usage

1. Add the **Sidestep** filter to your *content* source (Display Capture,
   Window Capture, Browser, …).
2. In the filter properties, pick the scene and the camera source.
3. Pick your preferred corners. Done.

Tip: give the camera scene item a Show/Hide transition (e.g. Fade) so hiding
and showing is smooth.

## Building

Requires CMake ≥ 3.16, a C11 compiler and libobs (OBS 31+) development files.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build          # core unit tests
sudo cmake --install build      # Linux: installs to the system plugin dir
```

The image-analysis and placement core (`src/core/`) has no libobs dependency;
build and test it alone with `-DSIDESTEP_BUILD_PLUGIN=OFF`.

## Licence

GPL-2.0-or-later (required for plugins linking libobs). See [`LICENSE`](LICENSE).
