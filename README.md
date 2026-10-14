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

## Installing

Download the package for your platform from the
[releases page](https://github.com/mitchelloharawild/obs-sidestep/releases):

- **Windows:** run the `.exe` installer, or unzip the `.zip` into your OBS
  installation folder.
- **macOS:** run the `.pkg` installer.
- **Ubuntu 24.04:** `sudo apt install ./obs-sidestep-*.deb`.

Then restart OBS.

## Building

The build system comes from
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
Requires CMake ≥ 3.28. On Linux it also needs libobs development files (OBS 31+)
and SIMDe (`sudo apt install obs-studio libsimde-dev` from the obsproject
PPA). On Windows and macOS, configuring downloads the OBS sources and
dependencies pinned in `buildspec.json`.

```sh
cmake --preset ubuntu-x86_64      # or: macos, windows-x64
cmake --build --preset ubuntu-x86_64
ctest --test-dir build_x86_64     # core unit tests
cd build_x86_64 && cpack -C RelWithDebInfo   # Linux: .deb in release/
```

Packages for all three platforms are built by GitHub Actions on every push;
pushing a version tag (e.g. `0.2.0`) creates a draft release with them
attached.

The image-analysis and placement core (`src/core/`) has no libobs dependency;
build and test it alone with `-DSIDESTEP_BUILD_PLUGIN=OFF`.

## Licence

MIT. See [`LICENSE`](LICENSE).
